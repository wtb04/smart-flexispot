#include "ble.h"

#include "ble_secrets.h"
#include "esp_check.h"
#include "proxy_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include <atomic>
#include <cstddef>
#include <cstring>

namespace ble {
namespace {

constexpr char TAG[] = "ble";

// Long enough not to forget a slowly advertising phone between packets. Ninety
// seconds was what a tenth-of-the-time scan needed to be reliable, and it made
// walking away take a minute and a half. Listening three times as often instead
// took the worst gap from twenty-three seconds to eight, so this is back to
// thirty -- nearly four times the worst gap seen rather than the seven seconds
// of margin it used to have. A phone that leaves does not fade, it just stops
// being heard, so this is also how long "away" takes.
constexpr TickType_t SEEN_TIMEOUT = pdMS_TO_TICKS(30000);

// The phone resolves from well outside the room, so distance has to come into
// it. Two thresholds, or the state flaps as the signal wanders across the line.
// Measured in the same window: smoothed between -66 and -51, never within ten
// dB of the lower threshold, so these were never what dropped it.
constexpr int RSSI_ENTER = -70;
constexpr int RSSI_EXIT  = -76;

// Packets swing ten dB as a body gets in the way, so the thresholds see this
// smoothed.
constexpr int RSSI_SMOOTHING = 3;  // of 10, weight given to the newest packet

// This part has no radio of its own -- Wi-Fi and Bluetooth both live on the
// companion chip -- so time spent listening is time not spent on Wi-Fi, and a
// tenth of the time was chosen to stay out of its way. It was too little:
// listening for a tenth, the phone was heard every six seconds at best and once
// went twenty-three without being heard at all, which is what kept dropping it.
// Three tenths, measured over the same kind of window: heard every two seconds,
// worst gap eight, and nothing over ten. No feed errors and no change in lookup
// times alongside it.
constexpr std::uint16_t SCAN_INTERVAL_MS = 1000;
constexpr std::uint16_t SCAN_WINDOW_MS   = 300;

std::atomic<bool> s_ready{false};

// Two copies of the key, as given and reversed: hex from Home Assistant and
// base64 from the Apple side are not always the same way round, and a key the
// wrong way round never matches, which looks exactly like an absent phone.
std::uint8_t s_irk[16]{};
std::uint8_t s_irk_reversed[16]{};
bool         s_have_irk     = false;
bool         s_use_reversed = false;
bool         s_order_known  = false;

std::atomic<TickType_t> s_seen{0};
std::atomic<int>        s_rssi{-127};
std::atomic<bool>       s_ever{false};
std::atomic<bool>       s_near{false};

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** 32 hex characters or 24 base64 ones; both turn up for the same key. */
bool parse_irk(const char *text, std::uint8_t out[16])
{
    if (text == nullptr) {
        return false;
    }
    char        stripped[64];
    std::size_t len = 0;
    for (const char *c = text; *c != '\0'; ++c) {
        if (*c == ':' || *c == ' ' || *c == '-') {
            continue;
        }
        if (len >= sizeof(stripped)) {
            return false;
        }
        stripped[len++] = *c;
    }

    if (len == 32) {
        for (std::size_t i = 0; i < 32; ++i) {
            const int value = hex_value(stripped[i]);
            if (value < 0) {
                return false;
            }
            if (i % 2 == 0) {
                out[i / 2] = static_cast<std::uint8_t>(value << 4);
            } else {
                out[i / 2] |= static_cast<std::uint8_t>(value);
            }
        }
        return true;
    }

    std::size_t decoded = 0;
    if (mbedtls_base64_decode(out, 16, &decoded,
                              reinterpret_cast<const unsigned char *>(stripped), len) == 0) {
        return decoded == 16;
    }
    return false;
}

/** A random address carries its kind in the top two bits of the last byte; 0b01
 *  is the resolvable private address a phone rotates through. */
bool is_resolvable_private(const ble_addr_t &addr)
{
    return addr.type == BLE_ADDR_RANDOM && (addr.val[5] & 0xC0) == 0x40;
}

bool hash_matches(const std::uint8_t key[16], const std::uint8_t val[6])
{
    // Plaintext is thirteen zero bytes then prand, most significant octet first;
    // NimBLE hands the address over least significant octet first.
    std::uint8_t block[16]{};
    block[13] = val[5];
    block[14] = val[4];
    block[15] = val[3];

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    std::uint8_t out[16]{};
    const bool   ok = mbedtls_aes_setkey_enc(&aes, key, 128) == 0 &&
                    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, out) == 0;
    mbedtls_aes_free(&aes);
    if (!ok) {
        return false;
    }
    // The bottom three octets of the result are the hash.
    return out[13] == val[2] && out[14] == val[1] && out[15] == val[0];
}

bool matches_irk(const std::uint8_t val[6])
{
    if (!s_have_irk) {
        return false;
    }
    if (s_order_known) {
        return hash_matches(s_use_reversed ? s_irk_reversed : s_irk, val);
    }
    if (hash_matches(s_irk, val)) {
        s_order_known  = true;
        s_use_reversed = false;
        ESP_LOGI(TAG, "identity key matched as given");
        return true;
    }
    if (hash_matches(s_irk_reversed, val)) {
        s_order_known  = true;
        s_use_reversed = true;
        ESP_LOGI(TAG, "identity key matched reversed");
        return true;
    }
    return false;
}

void on_resolved(int rssi)
{
    const bool first = !s_ever.exchange(true, std::memory_order_relaxed);
    const int  prev  = s_rssi.load(std::memory_order_relaxed);
    const int  avg =
        first ? rssi : (prev * (10 - RSSI_SMOOTHING) + rssi * RSSI_SMOOTHING) / 10;

    s_seen.store(xTaskGetTickCount(), std::memory_order_relaxed);
    s_rssi.store(avg, std::memory_order_relaxed);

    const bool near = s_near.load(std::memory_order_relaxed);
    if (!near && avg >= RSSI_ENTER) {
        s_near.store(true, std::memory_order_relaxed);
        ESP_LOGI(TAG, "phone near (%d dBm)", avg);
    } else if (near && avg < RSSI_EXIT) {
        s_near.store(false, std::memory_order_relaxed);
        ESP_LOGI(TAG, "phone far (%d dBm)", avg);
    }
}

void start_scanning();

int on_gap_event(ble_gap_event *event, void *)
{
    // The link to the desk proxy shares this host and these events.
    if (proxy::handle(event)) {
        return 0;
    }

    if (event->type == BLE_GAP_EVENT_DISC) {
        if (is_resolvable_private(event->disc.addr) && matches_irk(event->disc.addr.val)) {
            on_resolved(event->disc.rssi);
        } else if (proxy::consider(event->disc)) {
            s_ready.store(false, std::memory_order_relaxed);
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        ESP_LOGW(TAG, "scan ended (%d)", event->disc_complete.reason);
        s_ready.store(false, std::memory_order_relaxed);
    }
    return 0;
}

void start_scanning()
{
    ble_gap_disc_params params{};
    params.itvl              = SCAN_INTERVAL_MS * 1000 / 625;  // units of 0.625 ms
    params.window            = SCAN_WINDOW_MS * 1000 / 625;
    params.passive           = 1;  // no scan responses; the address is enough
    params.filter_duplicates = 0;  // duplicates carry a fresh RSSI, which is the point
    params.limited           = 0;
    params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;

    const int rc =
        ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, on_gap_event, nullptr);
    // Already scanning is the state this wants, not a failure. It happens
    // because scanning is asked for again whenever the desk link connects,
    // fails to connect or drops, and NimBLE may have resumed it already.
    if (rc == BLE_HS_EALREADY) {
        s_ready.store(true, std::memory_order_relaxed);
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed (%d)", rc);
        return;
    }
    s_ready.store(true, std::memory_order_relaxed);
    // Once, not every time the desk link comes and goes.
    static bool announced = false;
    if (!announced) {
        announced = true;
        ESP_LOGI(TAG, "scanning, %u ms window every %u ms", SCAN_WINDOW_MS, SCAN_INTERVAL_MS);
    }
}

void on_sync()
{
    proxy::set_rescan(start_scanning);
    start_scanning();
}

void on_reset(int reason)
{
    ESP_LOGW(TAG, "controller reset (%d)", reason);
    s_ready.store(false, std::memory_order_relaxed);
}

void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

esp_err_t start()
{
    s_have_irk = parse_irk(BLE_PHONE_IRK, s_irk);
    for (int i = 0; i < 16; ++i) {
        s_irk_reversed[i] = s_irk[15 - i];
    }
    if (s_have_irk) {
        ESP_LOGI(TAG, "identity key loaded");
    } else {
        ESP_LOGW(TAG, "no identity key - see ble_secrets.example.h");
    }

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init");
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    ESP_RETURN_ON_ERROR(proxy::start(), TAG, "proxy link");
    return ESP_OK;
}

Stats stats()
{
    Stats out;
    out.ready      = s_ready.load(std::memory_order_relaxed);
    out.has_key    = s_have_irk;
    out.ever_seen  = s_ever.load(std::memory_order_relaxed);
    out.phone_rssi = s_rssi.load(std::memory_order_relaxed);

    // Heard recently AND close enough: a phone walking out does not fade, it
    // just stops being heard.
    const TickType_t seen = s_seen.load(std::memory_order_relaxed);
    const bool heard = seen != 0 && (xTaskGetTickCount() - seen) <= SEEN_TIMEOUT;
    if (!heard) {
        s_near.store(false, std::memory_order_relaxed);
    }
    out.phone_present = heard && s_near.load(std::memory_order_relaxed);

    return out;
}

}  // namespace ble
