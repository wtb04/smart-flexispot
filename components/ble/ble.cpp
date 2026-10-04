#include "ble.h"

#include "app_state.h"

#include "esp_check.h"
#include "proxy_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

#if __has_include("ble_secrets.h")
#include "ble_secrets.h"
#endif
#ifndef BLE_PHONE_IRK
#define BLE_PHONE_IRK ""
#endif
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstring>

namespace ble {
namespace {
constexpr char TAG[]     = "ble";
constexpr char PRESENCE[] = "presence";

constexpr TickType_t SEEN_TIMEOUT = pdMS_TO_TICKS(30000);

constexpr int RSSI_ENTER = -70;
constexpr int RSSI_EXIT  = -76;

constexpr int RSSI_SMOOTHING       = 3;  // weight given to the newest packet
constexpr int RSSI_SMOOTHING_SCALE = 10;

constexpr std::uint16_t SCAN_INTERVAL_MS = 1000;
constexpr std::uint16_t SCAN_WINDOW_MS   = 300;
// While the screen is dark the phone is listened for a third as often: still
// every few seconds, well inside SEEN_TIMEOUT, for what Home Assistant does
// with the presence, at a third of the radio's time.
constexpr std::uint16_t DARK_INTERVAL_MS = 3000;
std::atomic<std::uint16_t> s_interval_ms{SCAN_INTERVAL_MS};
constexpr int           SCAN_UNIT_US     = 625;

constexpr std::size_t IRK_BYTES           = 16;
constexpr std::size_t HEX_DIGITS_PER_BYTE = 2;
constexpr std::size_t IRK_HEX_DIGITS      = IRK_BYTES * HEX_DIGITS_PER_BYTE;
constexpr std::size_t IRK_TEXT_MAX        = 64;
constexpr int         BITS_PER_HEX_DIGIT  = 4;
constexpr int         FIRST_HEX_LETTER    = 10;

// A resolvable private address is a random prand in its top three octets, with
// the kind in the top two bits, and a hash of the prand in the bottom three.
constexpr std::size_t  ADDR_BYTES           = sizeof(ble_addr_t::val);
constexpr std::size_t  HASH_BYTES           = 3;
constexpr std::size_t  PRAND_BYTES          = ADDR_BYTES - HASH_BYTES;
constexpr std::uint8_t ADDR_KIND_MASK       = 0xC0;
constexpr std::uint8_t ADDR_KIND_RESOLVABLE = 0x40;
constexpr std::size_t  AES_BLOCK_BYTES      = 16;

std::atomic<bool> s_ready{false};

std::uint8_t s_irk[IRK_BYTES]{};
// Two copies of the key, as given and reversed: hex from Home Assistant and
// base64 from the Apple side are not always the same way round, and a key the
// wrong way round looks exactly like an absent phone.
std::uint8_t s_irk_reversed[IRK_BYTES]{};
bool         s_have_irk     = false;
bool         s_use_reversed = false;
bool         s_order_known  = false;

std::atomic<TickType_t> s_seen{0};
std::atomic<int>        s_rssi{kNoRssi};
std::atomic<bool>       s_ever{false};
std::atomic<bool>       s_near{false};

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + FIRST_HEX_LETTER;
    if (c >= 'A' && c <= 'F') return c - 'A' + FIRST_HEX_LETTER;
    return -1;
}

bool parse_hex_irk(const char *digits, std::uint8_t out[IRK_BYTES])
{
    for (std::size_t i = 0; i < IRK_HEX_DIGITS; ++i) {
        const int value = hex_value(digits[i]);
        if (value < 0) {
            return false;
        }
        const std::size_t byte = i / HEX_DIGITS_PER_BYTE;
        if (i % HEX_DIGITS_PER_BYTE == 0) {
            out[byte] = static_cast<std::uint8_t>(value << BITS_PER_HEX_DIGIT);
        } else {
            out[byte] |= static_cast<std::uint8_t>(value);
        }
    }
    return true;
}

/** 32 hex characters or 24 base64 ones; both turn up for the same key. */
bool parse_irk(const char *text, std::uint8_t out[IRK_BYTES])
{
    if (text == nullptr) {
        return false;
    }
    char        stripped[IRK_TEXT_MAX];
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

    if (len == IRK_HEX_DIGITS) {
        return parse_hex_irk(stripped, out);
    }

    std::size_t decoded = 0;
    if (mbedtls_base64_decode(out, IRK_BYTES, &decoded,
                              reinterpret_cast<const unsigned char *>(stripped), len) == 0) {
        return decoded == IRK_BYTES;
    }
    return false;
}

bool is_resolvable_private(const ble_addr_t &addr)
{
    const std::uint8_t most_significant = addr.val[ADDR_BYTES - 1];
    return addr.type == BLE_ADDR_RANDOM &&
           (most_significant & ADDR_KIND_MASK) == ADDR_KIND_RESOLVABLE;
}

// Plaintext is zero bytes then prand, most significant octet first; NimBLE
// hands the address over least significant octet first. The bottom three
// octets of the result are the hash.
bool hash_matches(const std::uint8_t key[IRK_BYTES], const std::uint8_t val[ADDR_BYTES])
{
    const std::uint8_t *hash  = val;
    const std::uint8_t *prand = val + HASH_BYTES;

    std::uint8_t block[AES_BLOCK_BYTES]{};
    for (std::size_t i = 0; i < PRAND_BYTES; ++i) {
        block[AES_BLOCK_BYTES - 1 - i] = prand[i];
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    std::uint8_t out[AES_BLOCK_BYTES]{};
    const bool   ok = mbedtls_aes_setkey_enc(&aes, key, IRK_BYTES * CHAR_BIT) == 0 &&
                    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, out) == 0;
    mbedtls_aes_free(&aes);
    if (!ok) {
        return false;
    }
    for (std::size_t i = 0; i < HASH_BYTES; ++i) {
        if (out[AES_BLOCK_BYTES - 1 - i] != hash[i]) {
            return false;
        }
    }
    return true;
}

bool matches_irk(const std::uint8_t val[ADDR_BYTES])
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
        ESP_LOGI(PRESENCE, "identity key matched as given");
        return true;
    }
    if (hash_matches(s_irk_reversed, val)) {
        s_order_known  = true;
        s_use_reversed = true;
        ESP_LOGI(PRESENCE, "identity key matched reversed");
        return true;
    }
    return false;
}

void on_resolved(int rssi)
{
    const bool first = !s_ever.exchange(true, std::memory_order_relaxed);
    const int  prev  = s_rssi.load(std::memory_order_relaxed);
    const int  mixed = prev * (RSSI_SMOOTHING_SCALE - RSSI_SMOOTHING) + rssi * RSSI_SMOOTHING;
    const int  avg   = first ? rssi : mixed / RSSI_SMOOTHING_SCALE;

    s_seen.store(xTaskGetTickCount(), std::memory_order_relaxed);
    s_rssi.store(avg, std::memory_order_relaxed);

    const bool near = s_near.load(std::memory_order_relaxed);
    if (!near && avg >= RSSI_ENTER) {
        s_near.store(true, std::memory_order_relaxed);
        ESP_LOGI(PRESENCE, "phone near (%d dBm)", avg);
    } else if (near && avg < RSSI_EXIT) {
        s_near.store(false, std::memory_order_relaxed);
        ESP_LOGI(PRESENCE, "phone far (%d dBm)", avg);
    }
}

int on_gap_event(ble_gap_event *event, void *)
{
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

constexpr std::uint16_t scan_units(std::uint16_t ms)
{
    return static_cast<std::uint16_t>(ms * units::kUsPerMs / SCAN_UNIT_US);
}

void start_scanning()
{
    ble_gap_disc_params params{};
    params.itvl              = scan_units(s_interval_ms.load(std::memory_order_relaxed));
    params.window            = scan_units(SCAN_WINDOW_MS);
    params.passive           = 1;  // no scan responses; the address is enough
    params.filter_duplicates = 0;  // duplicates carry a fresh RSSI, which is the point
    params.limited           = 0;
    params.filter_policy     = BLE_HCI_SCAN_FILT_NO_WL;

    const int rc =
        ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, on_gap_event, nullptr);
    if (rc == BLE_HS_EALREADY) {
        s_ready.store(true, std::memory_order_relaxed);
        return;
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed (%d)", rc);
        return;
    }
    s_ready.store(true, std::memory_order_relaxed);
    static bool announced = false;
    if (!announced) {
        announced = true;
        ESP_LOGI(TAG, "scanning, %u ms window every %u ms", SCAN_WINDOW_MS, SCAN_INTERVAL_MS);
    }
}

// Scanning restarted from inside a disconnect can be refused while the old link
// is torn down, and nothing retried it. This puts it back every two seconds.
constexpr std::uint32_t RETRY_MS = 2000;
ble_npl_callout         s_retry;

void retry(ble_npl_event *)
{
    proxy::recover();
    if (!ble_gap_disc_active() && !ble_gap_conn_active()) {
        start_scanning();
    }
    ble_npl_callout_reset(&s_retry, ble_npl_time_ms_to_ticks32(RETRY_MS));
}

// On the host's own queue, so the scan is only ever started and stopped there.
ble_npl_event s_rescan;

void rescan(ble_npl_event *)
{
    if (ble_gap_conn_active()) {
        return;  // connecting to the desk; the retry starts scanning after, as it is
    }
    if (ble_gap_disc_active()) {
        ble_gap_disc_cancel();
    }
    start_scanning();
}

void on_sync()
{
    static bool armed = false;
    if (!armed) {
        armed = true;
        ble_npl_callout_init(&s_retry, nimble_port_get_dflt_eventq(), retry, nullptr);
        ble_npl_event_init(&s_rescan, rescan, nullptr);
    }
    proxy::set_rescan(start_scanning);
    start_scanning();
    ble_npl_callout_reset(&s_retry, ble_npl_time_ms_to_ticks32(RETRY_MS));
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

// Listens for the phone less often while the screen is dark.
void set_dark(bool dark)
{
    const std::uint16_t wanted = dark ? DARK_INTERVAL_MS : SCAN_INTERVAL_MS;
    if (s_interval_ms.exchange(wanted) != wanted && s_ready.load(std::memory_order_relaxed)) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_rescan);
    }
}

}  // namespace


esp_err_t start()
{
    set_dark(!app::get(app::Fact::ScreenOn));
    app::watch(app::Fact::ScreenOn, [](bool on) { set_dark(!on); });
    s_have_irk = parse_irk(BLE_PHONE_IRK, s_irk);
    std::reverse_copy(s_irk, s_irk + IRK_BYTES, s_irk_reversed);
    if (s_have_irk) {
        ESP_LOGI(PRESENCE, "identity key loaded");
    } else {
        ESP_LOGW(PRESENCE, "no identity key - see ble_secrets.example.h");
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

    const TickType_t seen = s_seen.load(std::memory_order_relaxed);
    const bool heard = seen != 0 && (xTaskGetTickCount() - seen) <= SEEN_TIMEOUT;
    if (!heard) {
        s_near.store(false, std::memory_order_relaxed);
    }
    out.phone_present = heard && s_near.load(std::memory_order_relaxed);

    return out;
}

}  // namespace ble
