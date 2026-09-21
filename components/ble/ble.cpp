#include "ble.h"

#include "ble_secrets.h"
#include "esp_check.h"
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

// Long enough that a phone which advertises slowly is not forgotten between
// packets, short enough that walking out of the room shows up promptly.
constexpr TickType_t SEEN_TIMEOUT = pdMS_TO_TICKS(30000);

// Presence is about the room, not the building: the phone resolves from well
// outside it, so distance has to come into it. Two thresholds rather than one,
// because a single line makes the state flap every time the signal wanders
// across it -- it has to be this strong to count as arriving, and stays
// counted until it falls clearly below that.
constexpr int RSSI_ENTER = -70;
constexpr int RSSI_EXIT  = -76;

// Smoothed before either threshold sees it. Individual packets swing ten dB or
// more as the phone moves or a body gets in the way, and deciding on raw
// packets would flap regardless of how far apart the thresholds are.
constexpr int RSSI_SMOOTHING = 3;  // of 10, weight given to the newest packet

// Scanning is continuous but not greedy. A phone advertises many times a
// second, so listening a tenth of the time still hears it within a second or
// two -- far inside the presence timeout -- while leaving the SDIO link it
// shares with Wi-Fi alone for the other ninety percent.
constexpr std::uint16_t SCAN_INTERVAL_MS = 1000;
constexpr std::uint16_t SCAN_WINDOW_MS   = 100;

std::atomic<bool> s_ready{false};

// The identity key, and what it has told us. Two copies of the key: as given
// and reversed. Hex from Home Assistant and base64 from the Apple side are the
// same key but not always the same way round, and a key the wrong way round
// never matches -- which looks exactly like the phone being absent.
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

/**
 * @brief Reads the identity key into 16 bytes, hex or base64.
 *
 * Both turn up in the wild for the same key -- 32 hex characters from Home
 * Assistant, 24 base64 ones from the Apple side -- so either is accepted
 * rather than making anybody convert between them by hand.
 */
bool parse_irk(const char *text, std::uint8_t out[16])
{
    if (text == nullptr) {
        return false;
    }
    char        stripped[64];
    std::size_t len = 0;
    for (const char *c = text; *c != '\0'; ++c) {
        if (*c == ':' || *c == ' ' || *c == '-') {
            continue;  // tolerate the separators people paste with
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

/**
 * @brief True for the addresses a phone rotates through.
 *
 * A random address carries its kind in the top two bits of the last byte:
 * 0b01 is a resolvable private address, which is what iOS and Android
 * broadcast to avoid being tracked by address alone.
 */
bool is_resolvable_private(const ble_addr_t &addr)
{
    return addr.type == BLE_ADDR_RANDOM && (addr.val[5] & 0xC0) == 0x40;
}

bool hash_matches(const std::uint8_t key[16], const std::uint8_t val[6])
{
    // Plaintext is thirteen zero bytes then prand, most significant octet
    // first; prand is the top half of the address and NimBLE hands the address
    // over least significant octet first, hence the reversal.
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
    // The bottom three octets of the result are the hash, against the bottom
    // half of the address.
    return out[13] == val[2] && out[14] == val[1] && out[15] == val[0];
}

bool matches_irk(const std::uint8_t val[6])
{
    if (!s_have_irk) {
        return false;
    }
    // Once one orientation has answered, stop paying for the other.
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

int on_gap_event(ble_gap_event *event, void *)
{
    if (event->type == BLE_GAP_EVENT_DISC) {
        if (is_resolvable_private(event->disc.addr) && matches_irk(event->disc.addr.val)) {
            on_resolved(event->disc.rssi);
        }
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        // Only reached if a duration was set; flagged so a stray stop cannot
        // pass for a working scan.
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
    params.passive           = 1;  // never ask for scan responses; the address is enough
    params.filter_duplicates = 0;  // duplicates carry a fresh RSSI, which is the point
    params.limited           = 0;
    params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;

    const int rc =
        ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, on_gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed (%d)", rc);
        return;
    }
    s_ready.store(true, std::memory_order_relaxed);
    ESP_LOGI(TAG, "scanning, %u ms window every %u ms", SCAN_WINDOW_MS, SCAN_INTERVAL_MS);
}

void on_sync() { start_scanning(); }

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
    return ESP_OK;
}

Stats stats()
{
    Stats out;
    out.ready      = s_ready.load(std::memory_order_relaxed);
    out.has_key    = s_have_irk;
    out.ever_seen  = s_ever.load(std::memory_order_relaxed);
    out.phone_rssi = s_rssi.load(std::memory_order_relaxed);

    // Heard recently AND close enough. Going quiet counts as leaving even if
    // the last packet was strong -- a phone in a pocket walking out does not
    // fade, it just stops being heard.
    const TickType_t seen = s_seen.load(std::memory_order_relaxed);
    const bool heard = seen != 0 && (xTaskGetTickCount() - seen) <= SEEN_TIMEOUT;
    if (!heard) {
        s_near.store(false, std::memory_order_relaxed);
    }
    out.phone_present = heard && s_near.load(std::memory_order_relaxed);
    return out;
}

}  // namespace ble
