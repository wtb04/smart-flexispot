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
#include "ical.h"
#include "media.h"
#include "radar.h"
#include "settings.h"
#include "power.h"
#include "room.h"
#include "ui.h"
#include "wifi.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>

namespace diagnostics {
namespace {
constexpr char TAG[] = "diag";

constexpr TickType_t TICK = pdMS_TO_TICKS(1000);

constexpr std::uint32_t TASK_STACK    = 3072;  // measured: uses 0.7 KB
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

void update_bluetooth()
{
    const ble::Stats     radio      = ble::stats();
    const ble::LinkStats link       = ble::link_stats();
    const bool           wants_link = settings::enabled(settings::Key::DeskBluetooth);

    push(Info::PhoneRadio, radio.ready ? "scanning" : "starting",
         radio.ready ? Level::Good : Level::Warn);

    push(Info::BleLink, link.connected ? "connected" : wants_link ? "searching" : "not needed",
         link.connected ? Level::Good : wants_link ? Level::Bad : Level::Neutral);

    char text[32];
    if (link.samples > 0) {
        std::snprintf(text, sizeof(text), "%d ms, worst %d", link.median_us / 1000,
                      link.max_us / 1000);
        push(Info::BleTrip, text);
        std::snprintf(text, sizeof(text), "%d of %d", link.lost, link.lost + link.samples);
        push(Info::BleLoss, text, link.lost == 0 ? Level::Good : Level::Warn);
    } else {
        push_missing(Info::BleTrip);
        push_missing(Info::BleLoss);
    }

    ui::set_health(ui::Subsystem::Bluetooth, !radio.ready ? Level::Bad
                                             : (wants_link && !link.connected) ? Level::Bad
                                                                               : Level::Good);
}

void update_presence()
{
    const ble::Stats radio = ble::stats();
    char             text[32];
    push(Info::PhoneKey, radio.has_key ? "loaded" : "missing",
         radio.has_key ? Level::Good : Level::Bad);

    ui::set_health(ui::Subsystem::Presence, !radio.has_key     ? Level::Bad
                                            : !radio.ever_seen ? Level::Warn
                                                               : Level::Good);

    if (!radio.ever_seen) {
        push(Info::PhoneState, "not seen");
        push_missing(Info::PhoneSignal);
        return;
    }

    push(Info::PhoneState, radio.phone_present ? "home" : "away");
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

    const char *status = battery.charging      ? "charging"
                         : battery.on_battery ? "discharging"
                         : battery.full       ? "full"
                                              : "idle";
    push(Info::PowerStatus, status);
}

void update_radar()
{
    radar::Status shot{};
    radar::status(shot);

    char text[48];
    if (shot.age_s < 0) {
        push_missing(Info::RadarFeed);
    } else {
        push(Info::RadarFeed, shot.ok ? "Answering" : "Refusing",
             shot.ok ? Level::Good : Level::Warn);
    }

    std::snprintf(text, sizeof(text), "%d aircraft", shot.count);
    push(Info::RadarAircraft, text);
    std::snprintf(text, sizeof(text), "%d km", shot.range_km);
    push(Info::RadarRange, text);

    if (shot.age_s < 0) {
        push_missing(Info::RadarSeen);
    } else {
        std::snprintf(text, sizeof(text), "%d s ago", shot.age_s);
        push(Info::RadarSeen, text, shot.age_s > 60 ? Level::Warn : Level::Neutral);
    }

    ui::set_health(ui::Subsystem::Radar, shot.age_s < 0            ? Level::Neutral
                                         : shot.ok && shot.age_s <= 60 ? Level::Good
                                                                       : Level::Warn);
}

void update_media()
{
    const media::Status status = media::status();

    push(Info::MediaPlayer, status.playing ? "Yes" : "Nothing");

    if (!status.have_art) {
        push(Info::MediaArt, "None");
    } else if (status.art_ok) {
        push(Info::MediaArt, "Shown", Level::Good);
    } else {
        push(Info::MediaArt, "Failed", Level::Warn);
    }

    if (status.decodes == 0) {
        push_missing(Info::MediaDecoder);
    } else {
        char text[48];
        std::snprintf(text, sizeof(text), "%s, %d ms",
                      status.hardware ? "Hardware" : "Software", status.decode_ms);
        push(Info::MediaDecoder, text);
    }

    ui::set_health(ui::Subsystem::Media,
                   status.have_art && !status.art_ok ? Level::Warn : Level::Good);
}

void update_calendar()
{
    static ical::Event ahead[8];
    const int          n = ical::upcoming(ahead, static_cast<int>(std::size(ahead)));

    char text[64];
    std::snprintf(text, sizeof(text), "%d", ical::kFeedCount);
    push(Info::CalFeeds, text);

    if (n == 0) {
        push_missing(Info::CalEvents);
        push_missing(Info::CalNext);
        ui::set_health(ui::Subsystem::Calendar, Level::Neutral);
        return;
    }

    std::snprintf(text, sizeof(text), "%d event%s", n, n == 1 ? "" : "s");
    push(Info::CalEvents, text);

    const auto   now     = static_cast<std::int64_t>(std::time(nullptr));
    const int    minutes = static_cast<int>((ahead[0].start - now) / 60);
    std::tm      local{};
    const auto   when = static_cast<std::time_t>(ahead[0].start);
    localtime_r(&when, &local);
    if (minutes > 0) {
        std::snprintf(text, sizeof(text), "%02d:%02d, in %dh%02dm", local.tm_hour, local.tm_min,
                      minutes / 60, minutes % 60);
    } else {
        std::snprintf(text, sizeof(text), "%02d:%02d, now", local.tm_hour, local.tm_min);
    }
    push(Info::CalNext, text);
    ui::set_health(ui::Subsystem::Calendar, Level::Good);
}

void update_desk()
{
    const bool over_ble = settings::enabled(settings::Key::DeskBluetooth);
    push(Info::DeskTransport, over_ble ? "Bluetooth" : "Local wire");

    bool              linked   = false;
    int               height   = -1;
    deskproto::Motion motion   = deskproto::Motion::Idle;
    const char       *state    = "silent";

    if (over_ble) {
        int  proxy_height = -1;
        bool box_linked   = false;
        if (!ble::desk::connected()) {
            state = "no proxy";
        } else if (!ble::desk::last(proxy_height, box_linked, motion)) {
            state = "proxy quiet";
        } else {
            linked = box_linked;
            height = proxy_height;
            state  = box_linked ? "responding" : "proxy up, box silent";
        }
    } else {
        linked = desk::linked();
        height = desk::height_mm();
        state  = linked ? "responding" : "silent";
    }

    ui::set_health(ui::Subsystem::Desk, linked ? Level::Good : Level::Bad);
    push(Info::DeskLink, state, linked ? Level::Good : Level::Bad);
    push_height(Info::DeskHeight, height);
    push(Info::DeskActive, desk::active_preset_label());
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

    const std::size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ui::set_health(ui::Subsystem::System, internal < 24 * 1024   ? Level::Bad
                                          : internal < 48 * 1024 ? Level::Warn
                                                                 : Level::Good);

    push_kilobytes(Info::SysRam, internal);
    push_kilobytes(Info::SysPsram, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    push_kilobytes(Info::SysRamLow, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

constexpr const char *NETWORK_TAGS[] = {
    "wifi",       "esp_netif_handlers", "esp_netif",  "esp_wifi_remote", "wifi_init",
    "H_API",      "H_SDIO_DRV",         "transport",  "sdio_wrapper",    "esp_hosted"};
constexpr const char *HASS_TAGS[] = {"hass",        "ha_ws",       "websocket_client", "room",
                                     "telemetry",   "mqtt_client", "MQTT_CLIENT",
                                     "transport_base"};
constexpr const char *BLUETOOTH_TAGS[] = {"NimBLE", "vhci_drv", "BTDM_INIT", "phy_init", "ble"};
constexpr const char *PRESENCE_TAGS[]  = {"presence"};
constexpr const char *POWER_TAGS[]     = {"power", "battery"};
constexpr const char *DESK_TAGS[]      = {"desk", "loctek", "desklink", "deskproxy", "proxy"};
constexpr const char *RADAR_TAGS[]     = {"radar"};
constexpr const char *MEDIA_TAGS[]     = {"media", "sound"};
constexpr const char *CALENDAR_TAGS[]  = {"ical"};
constexpr const char *SYSTEM_TAGS[]    = {
    "tab5",  "ui",     "diag",   "clock",     "settings",  "logbuf",   "board",
    "rtc",   "main_task", "cpu_start", "heap_init", "spiram", "esp_psram", "esp_image"};

struct TagSet {
    ui::Subsystem      subsystem;
    const char *const *tags;
    int                count;
};

constexpr TagSet TAG_SETS[] = {
    {ui::Subsystem::Network, NETWORK_TAGS, static_cast<int>(std::size(NETWORK_TAGS))},
    {ui::Subsystem::HomeAssistant, HASS_TAGS, static_cast<int>(std::size(HASS_TAGS))},
    {ui::Subsystem::Bluetooth, BLUETOOTH_TAGS, static_cast<int>(std::size(BLUETOOTH_TAGS))},
    {ui::Subsystem::Presence, PRESENCE_TAGS, static_cast<int>(std::size(PRESENCE_TAGS))},
    {ui::Subsystem::Power, POWER_TAGS, static_cast<int>(std::size(POWER_TAGS))},
    {ui::Subsystem::Desk, DESK_TAGS, static_cast<int>(std::size(DESK_TAGS))},
    {ui::Subsystem::Radar, RADAR_TAGS, static_cast<int>(std::size(RADAR_TAGS))},
    {ui::Subsystem::Media, MEDIA_TAGS, static_cast<int>(std::size(MEDIA_TAGS))},
    {ui::Subsystem::Calendar, CALENDAR_TAGS, static_cast<int>(std::size(CALENDAR_TAGS))},
    {ui::Subsystem::System, SYSTEM_TAGS, static_cast<int>(std::size(SYSTEM_TAGS))},
};
static_assert(std::size(TAG_SETS) == static_cast<std::size_t>(ui::Subsystem::Count),
              "a tag set per subsystem");

constexpr bool sets_in_order()
{
    for (std::size_t i = 0; i < std::size(TAG_SETS); ++i) {
        if (static_cast<std::size_t>(TAG_SETS[i].subsystem) != i) {
            return false;
        }
    }
    return true;
}
static_assert(sets_in_order(), "a tag set is indexed by its subsystem");

void update()
{
    update_network();
    update_radar();
    update_media();
    update_calendar();
    update_hass();
    update_bluetooth();
    update_presence();
    update_power();
    update_desk();
    update_system();
}

[[noreturn]] void diagnostics_task(void *)
{
    for (;;) {
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

int route(const char *tag)
{
    for (const TagSet &set : TAG_SETS) {
        for (int i = 0; i < set.count; ++i) {
            if (std::strcmp(tag, set.tags[i]) == 0) {
                return static_cast<int>(set.subsystem);
            }
        }
    }
    // Anything unrecognised is still worth keeping, and System is where somebody
    // would go looking for it.
    return static_cast<int>(ui::Subsystem::System);
}

int channels()
{
    return static_cast<int>(ui::Subsystem::Count);
}

int logs(ui::Subsystem subsystem, ui::LogLine *out, int max)
{
    const int channel = static_cast<int>(subsystem);
    const int held    = logbuf::count(channel);
    const int first   = held > max ? held - max : 0;

    int written = 0;
    for (int i = first; i < held; ++i) {
        logbuf::Entry line;
        if (!logbuf::at(channel, i, line)) {
            break;
        }
        out[written].level = line.level;
        std::snprintf(out[written].text, sizeof(out[written].text), "%s", line.text);
        ++written;
    }
    return written;
}

}  // namespace diagnostics
