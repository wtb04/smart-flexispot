#include "telemetry.h"

#include "board.h"
#include "desk.h"
#include "esp_check.h"
#include "ble.h"
#include "media.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_ws.h"
#include "hass.h"
#include "power.h"
#include "radar.h"
#include "room.h"
#include "settings.h"
#include "sound.h"
#include "ui.h"
#include "wifi.h"

#include <atomic>
#include <cstdio>

namespace telemetry {
namespace {

constexpr char TAG[] = "telemetry";

constexpr TickType_t PUBLISH_INTERVAL = pdMS_TO_TICKS(2000);

// How long to wait for an address before starting the network clients anyway.
// Generous: the C6 has to come up and associate, which is slower than on-die
// Wi-Fi. If it expires we start regardless, since both clients retry and the
// network may simply be late.
constexpr int NETWORK_WAIT_MS = 30000;

// Generous because of what this task does on the way through: it brings up
// MQTT, the WebSocket, BLE and the JPEG decoder at startup, and every tick it
// builds the state document with cJSON, which formats floats through full
// newlib printf -- well over a kilobyte of stack on its own.
constexpr std::uint32_t TASK_STACK    = 6144;  // measured: uses 3.0 KB
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

std::atomic<int> s_brightness{board::kDefaultBrightness};

// --- inbound, from Home Assistant. These run on the MQTT task. -------------

void on_preset(int preset)
{
    ESP_LOGI(TAG, "preset %d requested", preset);
    desk::on_preset(preset - 1, false);
}

void on_brightness(int percent)
{
    ESP_LOGI(TAG, "brightness %d%% requested", percent);
    s_brightness.store(percent, std::memory_order_relaxed);
    board::set_brightness_percent(percent);
    settings::set(settings::Key::Brightness, percent);
}

// Until the screens that use these exist, say what arrived so the entity ids
// and their states can be seen without guessing at them.
void on_entities(const hass::ws::EntityStore &store)
{
    room::render(store);
}

void on_move(hass::protocol::Move direction)
{
    ui::Move move = ui::Move::Stop;
    switch (direction) {
        case hass::protocol::Move::Up:   move = ui::Move::Up; break;
        case hass::protocol::Move::Down: move = ui::Move::Down; break;
        default:                         move = ui::Move::Stop; break;
    }
    ESP_LOGI(TAG, "move %d requested", static_cast<int>(move));
    desk::on_move(move);
}

void on_notify(const hass::protocol::Notification &notice)
{
    ESP_LOGI(TAG, "showing notification: '%s'", notice.message.c_str());
    sound::ding();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify(notice.title.c_str(), notice.message.c_str(),
                                             notice.level.c_str(), notice.timeout_ms));
}

// --- outbound ---------------------------------------------------------------

void fill_network(hass::protocol::Telemetry &out)
{
    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out.rssi_dbm = ap.rssi;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        char text[16];
        std::snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
        out.ip_address = text;
    }
}

void on_radar(const radar::Snapshot &snapshot)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar(snapshot));
}

void on_radar_details(const char *hex, const radar::Details &details)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar_details(hex, details));
}

void on_radar_photo(const char *hex, const void *pixels, int width, int height)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_radar_photo(hex, pixels, width, height));
}

void on_album_art(media::Art state, const void *pixels)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_album_art(pixels, state == media::Art::Failed));
}

[[noreturn]] void telemetry_task(void *)
{
    // Neither client can do anything without an address, and starting them
    // early only produces a burst of connection failures that make a real
    // fault harder to spot.
    if (!wifi::wait_for_ip(NETWORK_WAIT_MS)) {
        ESP_LOGW(TAG, "no address after %d s, starting clients anyway", NETWORK_WAIT_MS / 1000);
    }

    const hass::Handlers handlers{on_preset, on_brightness, on_notify, on_move};
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::start(handlers));
    // Not fatal: the desk works without Home Assistant.
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::start(on_entities));
    // Shares the SDIO link to the co-processor with Wi-Fi, so it goes up after
    // the network rather than racing it.
    ESP_ERROR_CHECK_WITHOUT_ABORT(ble::start());
    // Album art is fetched over plain HTTP from Home Assistant, so it needs the
    // network but nothing else.
    ESP_ERROR_CHECK_WITHOUT_ABORT(media::start(on_album_art));
    // Last: it needs an address and the home position, and the position comes
    // over the WebSocket once that is subscribed.
    ESP_ERROR_CHECK_WITHOUT_ABORT(radar::start(on_radar, on_radar_details, on_radar_photo));

    for (;;) {
        hass::protocol::Telemetry out;
        out.height_mm      = desk::height_mm();
        out.desk_connected = desk::linked();
        out.motion         = desk::motion();
        out.preset         = desk::active_preset();
        out.brightness     = s_brightness.load(std::memory_order_relaxed);
        out.uptime_s       = static_cast<std::uint32_t>(esp_timer_get_time() / 1000000);
        out.free_heap      = static_cast<std::uint32_t>(esp_get_free_heap_size());

        power::State battery{};
        if (power::read(battery) == ESP_OK && battery.present) {
            out.battery_percent   = battery.percent;
            out.battery_volts     = battery.bus_volts;
            out.battery_milliamps = static_cast<int>(battery.current_amps * 1000.0f);
            out.charging          = battery.charging;
            out.on_battery        = battery.on_battery;
        }

        if (wifi::connected()) {
            fill_network(out);
        }

        const ble::Stats radio = ble::stats();
        out.presence = radio.phone_present;
        // The last reading whenever the phone has ever been heard, not only
        // while it counts as present: the signal on the way out is exactly what
        // is worth looking at when the thresholds need moving.
        out.presence_rssi = radio.ever_seen ? radio.phone_rssi : -127;

        // Both, because the room page is fed by the WebSocket: a healthy
        // broker with a dead socket left stale temperatures and lights on
        // screen behind an icon saying everything was fine.
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_links(wifi::connected(), hass::connected() && hass::ws::connected()));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_presence(radio.has_key, radio.phone_present, radio.ever_seen));

        if (wifi::connected() && hass::connected() && hass::ws::connected()) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_done());
        }

        // Last, so everything gathered above is in the document. Returns an
        // error while the broker is unreachable, which is normal and not worth
        // logging every two seconds.
        hass::publish(out);

        // Said once, when everything that is going to start has started. This is
        // the number that decides whether a TLS handshake or a Bluetooth packet
        // can find a buffer, and it is worth knowing without waiting for it to
        // go wrong.
        static bool settled = false;
        if (!settled && esp_timer_get_time() > 40000000) {
            settled = true;
            ESP_LOGI(TAG, "memory once up: %u KB dma-capable, %u KB internal, %u KB internal low",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     static_cast<unsigned>(
                         heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
        }

        // What ran out when the Bluetooth transport and TLS both wanted DMA
        // memory at once. Reported when it is low rather than all the time, so
        // it says something when it appears.
        static std::int64_t complained = 0;
        const std::size_t   dma_free   = heap_caps_get_free_size(MALLOC_CAP_DMA);
        const std::size_t   internal   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        // Set from what actually broke rather than from what felt comfortable:
        // allocations began failing around eleven kilobytes and the panel was
        // still steady at thirty-five, so the line is drawn between them.
        if (dma_free < 24 * 1024 && esp_timer_get_time() - complained > 30000000) {
            complained = esp_timer_get_time();
            ESP_LOGW(TAG, "low memory: %u KB dma-capable, %u KB internal",
                     static_cast<unsigned>(dma_free / 1024),
                     static_cast<unsigned>(internal / 1024));
        }

        vTaskDelay(PUBLISH_INTERVAL);
    }
}

}  // namespace

esp_err_t start()
{
    TaskHandle_t task = xTaskCreateStaticPinnedToCore(telemetry_task, "telemetry", TASK_STACK,
                                                      nullptr, TASK_PRIORITY, s_task_stack,
                                                      &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void note_brightness(int percent)
{
    s_brightness.store(percent, std::memory_order_relaxed);
}

}  // namespace telemetry
