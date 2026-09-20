#include "telemetry.h"

#include "board.h"
#include "desk.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hass.h"
#include "power.h"
#include "sound.h"
#include "ui.h"
#include "wifi.h"

#include <atomic>
#include <cstdio>

namespace telemetry {
namespace {

constexpr char TAG[] = "telemetry";

constexpr TickType_t PUBLISH_INTERVAL = pdMS_TO_TICKS(2000);

constexpr std::uint32_t TASK_STACK    = 4096;
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

[[noreturn]] void telemetry_task(void *)
{
    for (;;) {
        hass::protocol::Telemetry out;
        out.height_mm      = desk::height_mm();
        out.desk_connected = desk::linked();
        out.motion         = desk::motion();
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

        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_links(wifi::connected(), hass::connected()));

        // Returns an error while the broker is unreachable, which is normal and
        // not worth logging every two seconds.
        hass::publish(out);
        vTaskDelay(PUBLISH_INTERVAL);
    }
}

}  // namespace

esp_err_t start()
{
    const hass::Handlers handlers{on_preset, on_brightness, on_notify, on_move};
    ESP_RETURN_ON_ERROR(hass::start(handlers), TAG, "mqtt");

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
