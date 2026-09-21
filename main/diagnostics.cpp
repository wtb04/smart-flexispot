#include "diagnostics.h"

#include "ble.h"
#include "desk.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_ws.h"
#include "hass.h"
#include "logbuf.h"
#include "power.h"
#include "room.h"
#include "ui.h"
#include "wifi.h"

#include <cstdio>
#include <cstring>
#include <iterator>

namespace diagnostics {
namespace {

constexpr char TAG[] = "diag";

// Only ever runs while the view is on screen, so a second costs nothing when it
// is not and keeps the uptime honest when it is.
constexpr TickType_t TICK = pdMS_TO_TICKS(1000);

constexpr std::uint32_t TASK_STACK    = 4096;
constexpr UBaseType_t   TASK_PRIORITY = 1;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

using ui::Info;
using ui::Level;

void push(Info field, const char *value, Level level = Level::Neutral)
{
    ui::set_info(field, value, level);
}

void push_missing(Info field)
{
    push(field, "--");
}

Level signal_level(int rssi_dbm)
{
    if (rssi_dbm >= -60) {
        return Level::Good;
    }
    return rssi_dbm >= -75 ? Level::Warn : Level::Bad;
}

void push_link(Info field, bool up)
{
    push(field, up ? "connected" : "offline", up ? Level::Good : Level::Bad);
}

void push_height(Info field, int height_mm)
{
    if (height_mm < 0) {
        push(field, "not learned");
        return;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d.%d cm", height_mm / 10, height_mm % 10);
    push(field, text);
}

void push_kilobytes(Info field, std::size_t bytes)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%u KB", static_cast<unsigned>(bytes / 1024));
    push(field, text);
}

void update_network()
{
    const bool up = wifi::connected();
    push_link(Info::WifiState, up);
    ui::set_health(ui::Subsystem::Network, up ? Level::Good : Level::Bad);
    if (!up) {
        push_missing(Info::WifiSsid);
        push_missing(Info::WifiIp);
        push_missing(Info::WifiSignal);
        push_missing(Info::WifiChannel);
    }

    char text[32];
    std::uint8_t mac[6]{};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
                      mac[3], mac[4], mac[5]);
        push(Info::WifiMac, text);
    } else {
        push_missing(Info::WifiMac);
    }

    if (!up) {
        return;
    }

    wifi_ap_record_t ap{};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        push(Info::WifiSsid, reinterpret_cast<const char *>(ap.ssid));
        std::snprintf(text, sizeof(text), "%d dBm", ap.rssi);
        push(Info::WifiSignal, text, signal_level(ap.rssi));
        std::snprintf(text, sizeof(text), "%d", ap.primary);
        push(Info::WifiChannel, text);
    }

    esp_netif_t        *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        std::snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
        push(Info::WifiIp, text);
    } else {
        push_missing(Info::WifiIp);
    }
}

void update_hass()
{
    push_link(Info::HaBroker, hass::connected());
    push_link(Info::HaSocket, hass::ws::connected());

    const int entities = room::entity_count();
    char      text[12];
    std::snprintf(text, sizeof(text), "%d", entities);
    push(Info::HaEntities, text, entities > 0 ? Level::Good : Level::Warn);

    const int links = static_cast<int>(hass::connected()) + static_cast<int>(hass::ws::connected());
    ui::set_health(ui::Subsystem::HomeAssistant,
                   links == 2 ? Level::Good : links == 1 ? Level::Warn : Level::Bad);
}

void update_phone()
{
    const ble::Stats radio = ble::stats();
    push(Info::PhoneRadio, radio.ready ? "scanning" : "starting",
         radio.ready ? Level::Good : Level::Warn);
    push(Info::PhoneKey, radio.has_key ? "loaded" : "missing",
         radio.has_key ? Level::Good : Level::Bad);

    // Health is whether the tracker is doing its job, not whether the answer is
    // the one you wanted: a phone correctly seen to be away is working
    // perfectly. Only a dead radio, a missing key or a phone never once
    // recognised say something is wrong.
    ui::set_health(ui::Subsystem::Phone, !radio.ready         ? Level::Bad
                                         : !radio.has_key     ? Level::Bad
                                         : !radio.ever_seen   ? Level::Warn
                                                              : Level::Good);

    if (!radio.ever_seen) {
        push(Info::PhoneState, "not seen");
        push_missing(Info::PhoneSignal);
        return;
    }

    push(Info::PhoneState, radio.phone_present ? "home" : "away");
    char text[16];
    std::snprintf(text, sizeof(text), "%d dBm", radio.phone_rssi);
    push(Info::PhoneSignal, text);
}

void update_power()
{
    power::State battery{};
    if (!power::last(battery)) {
        ui::set_health(ui::Subsystem::Power, Level::Bad);
        push(Info::PowerSource, "no monitor", Level::Bad);
        push_missing(Info::PowerCharge);
        push_missing(Info::PowerVolts);
        push_missing(Info::PowerCurrent);
        push_missing(Info::PowerStatus);
        return;
    }

    if (!battery.present) {
        ui::set_health(ui::Subsystem::Power, Level::Good);
        push(Info::PowerSource, "USB");
        push_missing(Info::PowerCharge);
        push_missing(Info::PowerVolts);
        push_missing(Info::PowerCurrent);
        push(Info::PowerStatus, "no pack");
        return;
    }

    // Running off the pack is not a fault; running it flat is.
    ui::set_health(ui::Subsystem::Power,
                   !battery.present                              ? Level::Warn
                   : battery.on_battery && battery.percent < 10  ? Level::Bad
                   : battery.on_battery && battery.percent < 25  ? Level::Warn
                                                                 : Level::Good);

    push(Info::PowerSource, battery.on_battery ? "battery" : "USB");

    char text[16];
    std::snprintf(text, sizeof(text), "%d%%", battery.percent);
    push(Info::PowerCharge, text,
         battery.percent >= 40 ? Level::Good : battery.percent >= 15 ? Level::Warn : Level::Bad);

    std::snprintf(text, sizeof(text), "%.2f V", battery.bus_volts);
    push(Info::PowerVolts, text);

    std::snprintf(text, sizeof(text), "%d mA", static_cast<int>(battery.current_amps * 1000.0f));
    push(Info::PowerCurrent, text);

    const char *status = battery.charging ? "charging" : battery.on_battery ? "discharging" : "idle";
    push(Info::PowerStatus, status);
}

const char *preset_label(const char *preset)
{
    if (std::strcmp(preset, "stand") == 0) {
        return "Stand";
    }
    if (std::strcmp(preset, "sit") == 0) {
        return "Sit";
    }
    if (std::strcmp(preset, "preset_1") == 0) {
        return "Preset 1";
    }
    if (std::strcmp(preset, "preset_2") == 0) {
        return "Preset 2";
    }
    return "between";
}

void update_desk()
{
    const bool linked = desk::linked();
    ui::set_health(ui::Subsystem::Desk, linked ? Level::Good : Level::Bad);
    push(Info::DeskLink, linked ? "responding" : "silent", linked ? Level::Good : Level::Bad);
    push_height(Info::DeskHeight, desk::height_mm());
    push(Info::DeskActive, preset_label(desk::active_preset()));
    push_height(Info::DeskStand, desk::preset_height_mm(2));
    push_height(Info::DeskSit, desk::preset_height_mm(3));
    push_height(Info::DeskOne, desk::preset_height_mm(0));
    push_height(Info::DeskTwo, desk::preset_height_mm(1));
}

void update_system()
{
    const esp_app_desc_t *app = esp_app_get_description();
    push(Info::SysFirmware, app->version);

    char text[40];
    std::snprintf(text, sizeof(text), "%s %s", app->date, app->time);
    push(Info::SysBuilt, text);

    const unsigned seconds = static_cast<unsigned>(esp_timer_get_time() / 1000000);
    if (seconds >= 86400) {
        std::snprintf(text, sizeof(text), "%ud %uh", seconds / 86400, (seconds % 86400) / 3600);
    } else if (seconds >= 3600) {
        std::snprintf(text, sizeof(text), "%uh %um", seconds / 3600, (seconds % 3600) / 60);
    } else {
        std::snprintf(text, sizeof(text), "%um %us", seconds / 60, seconds % 60);
    }
    push(Info::SysUptime, text);

    // The panel runs out of internal RAM long before it runs out of PSRAM, so
    // that is the number worth watching.
    const std::size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ui::set_health(ui::Subsystem::System, internal < 24 * 1024   ? Level::Bad
                                          : internal < 48 * 1024 ? Level::Warn
                                                                 : Level::Good);

    push_kilobytes(Info::SysRam, internal);
    push_kilobytes(Info::SysPsram, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    push_kilobytes(Info::SysRamLow, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

// Which tags belong to which tile. A subsystem is more than one component --
// the network is as much the co-processor link as it is the Wi-Fi driver.
constexpr const char *NETWORK_TAGS[] = {
    "wifi",       "esp_netif_handlers", "esp_netif",  "esp_wifi_remote", "wifi_init",
    "H_API",      "H_SDIO_DRV",         "transport",  "sdio_wrapper",    "esp_hosted"};
constexpr const char *HASS_TAGS[] = {"hass", "ha_ws",     "websocket_client",
                                     "room", "telemetry", "mqtt_client",
                                     "MQTT_CLIENT",       "transport_base"};
constexpr const char *PHONE_TAGS[]  = {"ble", "NimBLE", "vhci_drv"};
constexpr const char *POWER_TAGS[]  = {"power", "battery"};
constexpr const char *DESK_TAGS[]   = {"desk", "loctek"};
constexpr const char *SYSTEM_TAGS[] = {"tab5",  "ui",       "diag",   "media", "clock",
                                       "sound", "settings", "logbuf", "board", "main_task",
                                       "cpu_start", "heap_init", "spiram", "esp_psram", "esp_image"};

struct TagSet {
    const char        *subsystem;
    const char *const *tags;
    int                count;
};

constexpr TagSet TAG_SETS[] = {
    {"Network", NETWORK_TAGS, static_cast<int>(std::size(NETWORK_TAGS))},
    {"Home Assistant", HASS_TAGS, static_cast<int>(std::size(HASS_TAGS))},
    {"Phone", PHONE_TAGS, static_cast<int>(std::size(PHONE_TAGS))},
    {"Power", POWER_TAGS, static_cast<int>(std::size(POWER_TAGS))},
    {"Desk", DESK_TAGS, static_cast<int>(std::size(DESK_TAGS))},
    {"System", SYSTEM_TAGS, static_cast<int>(std::size(SYSTEM_TAGS))},
};

void update()
{
    update_network();
    update_hass();
    update_phone();
    update_power();
    update_desk();
    update_system();
}

[[noreturn]] void diagnostics_task(void *)
{
    for (;;) {
        // Woken when the view opens so it is never filled in behind the user,
        // and otherwise once a second while it is up.
        ulTaskNotifyTake(pdTRUE, TICK);
        if (ui::diagnostics_open()) {
            update();
        }
    }
}

}  // namespace

esp_err_t start()
{
    s_task = xTaskCreateStaticPinnedToCore(diagnostics_task, "diag", TASK_STACK, nullptr,
                                           TASK_PRIORITY, s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void refresh()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void logs(const char *subsystem, char *out, std::size_t size)
{
    for (const TagSet &set : TAG_SETS) {
        if (std::strcmp(set.subsystem, subsystem) == 0) {
            logbuf::recent(set.tags, set.count, out, size, 40);
            return;
        }
    }
    if (size > 0) {
        out[0] = '\0';
    }
}

}  // namespace diagnostics
