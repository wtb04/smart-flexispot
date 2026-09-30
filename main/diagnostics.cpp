#include "board.h"
#include "diagnostics.h"

#include "ble.h"
#include "ble_desk.h"
#include "desk.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha_ws.h"
#include "travel.h"
#include "hass.h"
#include "logbuf.h"
#include "ical.h"
#include "jobs.h"
#include "media.h"
#include "net_stream.h"
#include "radar.h"
#include "settings.h"
#include "power.h"
#include "room.h"
#include "ui.h"
#include "units.h"
#include "wifi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>

namespace diagnostics {
namespace {
constexpr char TAG[] = "diag";

constexpr int TICK_MS = units::kMsPerSecond;

constexpr int STRONG_SIGNAL_DBM = -60;
constexpr int FAIR_SIGNAL_DBM   = -75;

constexpr int REFUSAL_RECENT_S       = 10 * units::kSecondsPerMinute;
constexpr int DESK_HEARD_RECENTLY_MS = 2 * units::kMsPerSecond;
constexpr int MS_PER_TENTH_SECOND    = units::kMsPerSecond / 10;
constexpr int RADAR_STALE_S          = units::kSecondsPerMinute;

constexpr int CHARGE_GOOD_PERCENT         = 40;
constexpr int CHARGE_LOW_PERCENT          = 15;
constexpr int ON_BATTERY_LOW_PERCENT      = 25;
constexpr int ON_BATTERY_CRITICAL_PERCENT = 10;

constexpr float MILLIAMPS_PER_AMP = 1000.0f;

constexpr std::size_t INTERNAL_RAM_LOW_BYTES   = 24 * units::kBytesPerKiB;
constexpr std::size_t INTERNAL_RAM_TIGHT_BYTES = 48 * units::kBytesPerKiB;

constexpr int CALENDAR_LOOKAHEAD_EVENTS = 8;

constexpr std::size_t VALUE_TEXT_SIZE     = 16;
constexpr std::size_t ROW_TEXT_SIZE       = 32;
constexpr std::size_t REFUSAL_TEXT_SIZE   = 96;
constexpr std::size_t REFUSAL_REASON_SIZE = 32;
constexpr std::size_t AGO_TEXT_SIZE       = 24;
constexpr std::size_t CALENDAR_TEXT_SIZE  = 48;
constexpr std::size_t CLOCK_TEXT_SIZE     = 8;
constexpr std::size_t SYSTEM_TEXT_SIZE    = 40;

jobs::Job s_job = jobs::kNoJob;

using ui::Level;

// ---- The cards, in the order they are drawn: three rows of three. ----

enum Card : int { WIFI, HASS, PRESENCE, DESK, LINK, POWER, RADAR, CALENDAR, SYSTEM, CARDS };

enum WifiRow : int { WIFI_NETWORK, WIFI_SIGNAL, WIFI_ADDRESS, WIFI_CHANNEL, WIFI_MAC };
constexpr const char *WIFI_ROWS[] = {"Network", "Signal", "Address", "Channel", "MAC"};

enum HassRow : int { HASS_BROKER, HASS_SOCKET, HASS_JELLYFIN, HASS_ENTITIES, HASS_ART, HASS_REFUSED };
constexpr const char *HASS_ROWS[] = {"Broker", "Socket", "Jellyfin", "Entities", "Cover art", "Last refused"};

enum PresenceRow : int { PRESENCE_PHONE, PRESENCE_SIGNAL, PRESENCE_KEY, PRESENCE_RADIO };
constexpr const char *PRESENCE_ROWS[] = {"Phone", "Signal", "Identity key", "Radio"};

enum DeskRow : int { DESK_HEIGHT, DESK_AT, DESK_MOVING, DESK_PRESET };  // then the rest of the presets
const char *DESK_ROWS[DESK_PRESET + deskproto::kPresetCount] = {"Height", "Standing at",
                                                                "Doing"};

enum LinkRow : int { LINK_OVER, LINK_BOX, LINK_COMPANION, LINK_TRIP, LINK_HEARD };
constexpr const char *LINK_ROWS[] = {"Driven over", "Control box", "Companion", "Round trip",
                                     "Last heard"};

enum PowerRow : int { POWER_SOURCE, POWER_CHARGE, POWER_VOLTS, POWER_CURRENT, POWER_STATE };
constexpr const char *POWER_ROWS[] = {"Source", "Charge", "Voltage", "Current", "State"};

enum RadarRow : int { RADAR_FEED, RADAR_PLANES, RADAR_REACH, RADAR_SWEEP };
constexpr const char *RADAR_ROWS[] = {"Feed", "Planes", "Reach", "Last sweep"};

enum CalendarRow : int { CAL_FEEDS, CAL_AHEAD, CAL_NEXT, CAL_JOURNEY };
constexpr const char *CALENDAR_ROWS[] = {"Feeds", "Ahead", "Next", "Journey"};

enum SystemRow : int {
    SYS_FIRMWARE, SYS_BUILT, SYS_UPTIME, SYS_INTERNAL, SYS_LARGEST, SYS_PSRAM, SYS_LOW, SYS_JPEG
};
constexpr const char *SYSTEM_ROWS[] = {"Firmware",      "Built",     "Uptime",
                                       "Internal free", "Largest block", "PSRAM free",
                                       "Low mark",      "JPEG decoder"};

template <std::size_t N>
constexpr int rows_of(const char *const (&)[N])
{
    return static_cast<int>(N);
}

const ui::Card CARDS_TABLE[CARDS] = {
    {"Wi-Fi", ui::Glyph::Wifi, WIFI_ROWS, rows_of(WIFI_ROWS)},
    {"Home Assistant", ui::Glyph::Home, HASS_ROWS, rows_of(HASS_ROWS)},
    {"Presence", ui::Glyph::Presence, PRESENCE_ROWS, rows_of(PRESENCE_ROWS)},
    {"Desk", ui::Glyph::Desk, DESK_ROWS, rows_of(DESK_ROWS)},
    {"Desk link", ui::Glyph::Link, LINK_ROWS, rows_of(LINK_ROWS)},
    {"Power", ui::Glyph::Power, POWER_ROWS, rows_of(POWER_ROWS)},
    {"Radar", ui::Glyph::Radar, RADAR_ROWS, rows_of(RADAR_ROWS)},
    {"Calendar", ui::Glyph::Calendar, CALENDAR_ROWS, rows_of(CALENDAR_ROWS)},
    {"System", ui::Glyph::System, SYSTEM_ROWS, rows_of(SYSTEM_ROWS)},
};
static_assert(CARDS <= ui::kMaxCards, "the grid holds three by three");

void row(Card card, int which, const char *value, Level level = Level::Neutral)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_row(card, which, value, level));
}

void summary(Card card, const char *value, Level level)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_card(card, value, level));
}

Level signal_level(int rssi_dbm)
{
    if (rssi_dbm >= STRONG_SIGNAL_DBM) {
        return Level::Good;
    }
    return rssi_dbm >= FAIR_SIGNAL_DBM ? Level::Warn : Level::Bad;
}

void height_text(char *out, std::size_t size, int height_mm)
{
    if (height_mm < 0) {
        std::snprintf(out, size, "not set");
    } else {
        std::snprintf(out, size, "%d.%d cm", height_mm / units::kMmPerCm,
                      height_mm % units::kMmPerCm);
    }
}

unsigned kib(std::size_t bytes)
{
    return static_cast<unsigned>(bytes / units::kBytesPerKiB);
}

int ms_from_us(int us)
{
    return us / static_cast<int>(units::kUsPerMs);
}

void kilobytes(Card card, int which, std::size_t bytes, Level level = Level::Neutral)
{
    char text[VALUE_TEXT_SIZE];
    std::snprintf(text, sizeof(text), "%u KB", kib(bytes));
    row(card, which, text, level);
}

void ago(char *out, std::size_t size, int seconds)
{
    if (seconds < units::kSecondsPerMinute) {
        std::snprintf(out, size, "%d s ago", seconds);
    } else if (seconds < units::kSecondsPerHour) {
        std::snprintf(out, size, "%d min ago", seconds / units::kSecondsPerMinute);
    } else {
        std::snprintf(out, size, "%d h ago", seconds / units::kSecondsPerHour);
    }
}

// ---- What each card says. ----

void update_wifi()
{
    const wifi::Info info = wifi::info();
    char             text[ROW_TEXT_SIZE];
    row(WIFI, WIFI_MAC, info.mac[0] != '\0' ? info.mac : nullptr);
    if (!info.connected) {
        summary(WIFI, "offline", Level::Bad);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::Wifi, "offline"));
        row(WIFI, WIFI_NETWORK, "not joined", Level::Bad);
        row(WIFI, WIFI_SIGNAL, nullptr);
        row(WIFI, WIFI_ADDRESS, nullptr);
        row(WIFI, WIFI_CHANNEL, nullptr);
        return;
    }
    row(WIFI, WIFI_NETWORK, info.have_ap ? info.ssid : nullptr);
    row(WIFI, WIFI_ADDRESS, info.ip[0] != '\0' ? info.ip : nullptr);
    if (info.have_ap) {
        std::snprintf(text, sizeof(text), "%d dBm", info.rssi_dbm);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::Wifi, text));
        row(WIFI, WIFI_SIGNAL, text, signal_level(info.rssi_dbm));
        summary(WIFI, text, signal_level(info.rssi_dbm));
        std::snprintf(text, sizeof(text), "%d", info.channel);
        row(WIFI, WIFI_CHANNEL, text);
    } else {
        summary(WIFI, "joined", Level::Good);
    }
}

// How one of net's live connections is doing, as "again in 8 s, token rejected".
void stream_row(Card card, int which, const char *name)
{
    const net::Stream stream = net::find_stream(name);
    if (stream == net::kNoStream) {
        row(card, which, "not set up");
        return;
    }
    const net::StreamStatus status = net::stream_status(stream);
    char                    text[REFUSAL_TEXT_SIZE];
    switch (status.state) {
        case net::StreamState::Ready: row(card, which, "connected", Level::Good); return;
        case net::StreamState::Connecting:
        case net::StreamState::Open: row(card, which, "connecting", Level::Warn); return;
        case net::StreamState::Offline: row(card, which, "no network", Level::Bad); return;
        case net::StreamState::Waiting:
            std::snprintf(text, sizeof(text), "again in %d s%s%s", (status.next_in_ms + 999) / 1000,
                          status.last_error.empty() ? "" : ", ", status.last_error.c_str());
            row(card, which, text, Level::Bad);
            return;
    }
}

void update_hass()
{
    const bool broker = hass::connected();
    const bool socket = hass::ws::connected();
    stream_row(HASS, HASS_BROKER, "hass");
    stream_row(HASS, HASS_SOCKET, "ha_ws");
    stream_row(HASS, HASS_JELLYFIN, "jellyfin");
    summary(HASS,
            broker && socket ? "connected"
            : broker         ? "broker only"
            : socket         ? "socket only"
                             : "offline",
            broker && socket ? Level::Good : broker || socket ? Level::Warn : Level::Bad);

    char      text[REFUSAL_TEXT_SIZE];
    const int entities = room::entity_count();
    std::snprintf(text, sizeof(text), "%d", entities);
    row(HASS, HASS_ENTITIES, text, entities > 0 ? Level::Good : Level::Warn);

    const media::Status art = media::status();
    row(HASS, HASS_ART, !art.have_art ? "none" : art.art_ok ? "shown" : "failed",
        art.have_art && !art.art_ok ? Level::Warn : Level::Neutral);

    char reason[REFUSAL_REASON_SIZE];
    int  age = 0;
    if (hass::ws::last_refusal(reason, sizeof(reason), age)) {
        char when[AGO_TEXT_SIZE];
        ago(when, sizeof(when), age);
        std::snprintf(text, sizeof(text), "%s, %s", reason, when);
        row(HASS, HASS_REFUSED, text, age < REFUSAL_RECENT_S ? Level::Warn : Level::Neutral);
    } else {
        row(HASS, HASS_REFUSED, "none");
    }
}

void update_presence()
{
    const ble::Stats radio = ble::stats();
    row(PRESENCE, PRESENCE_KEY, radio.has_key ? "loaded" : "missing",
        radio.has_key ? Level::Good : Level::Bad);
    row(PRESENCE, PRESENCE_RADIO, radio.ready ? "scanning" : "starting",
        radio.ready ? Level::Good : Level::Warn);

    if (!radio.has_key) {
        summary(PRESENCE, "no key", Level::Bad);
    } else if (!radio.ever_seen) {
        summary(PRESENCE, "not seen", Level::Warn);
    } else {
        summary(PRESENCE, radio.phone_present ? "home" : "away", Level::Good);
    }

    if (!radio.ever_seen) {
        row(PRESENCE, PRESENCE_PHONE, "not seen");
        row(PRESENCE, PRESENCE_SIGNAL, nullptr);
        return;
    }
    row(PRESENCE, PRESENCE_PHONE, radio.phone_present ? "home" : "away");
    char text[VALUE_TEXT_SIZE];
    std::snprintf(text, sizeof(text), "%d dBm", radio.phone_rssi);
    row(PRESENCE, PRESENCE_SIGNAL, text);
}

void update_desk()
{
    char      text[VALUE_TEXT_SIZE];
    const int height = desk::height_mm();
    height_text(text, sizeof(text), height);
    row(DESK, DESK_HEIGHT, height >= 0 ? text : nullptr);
    // The link card says why it is not linked; this one only what it knows.
    summary(DESK, height >= 0 && desk::linked() ? text : "--",
            desk::linked() ? Level::Good : Level::Neutral);

    row(DESK, DESK_AT, ui::preset_name(desk::active_preset_index()));
    const char *motion = desk::motion();
    row(DESK, DESK_MOVING,
        std::strcmp(motion, "moving_up") == 0     ? "going up"
        : std::strcmp(motion, "moving_down") == 0 ? "going down"
                                                  : "still");
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        height_text(text, sizeof(text), desk::preset_height_mm(i));
        row(DESK, DESK_PRESET + i, text);
    }
}

void update_wire_link()
{
    const bool linked = desk::linked();
    row(LINK, LINK_BOX, linked ? "answering" : "silent", linked ? Level::Good : Level::Bad);
    row(LINK, LINK_COMPANION, "not used");
    row(LINK, LINK_TRIP, nullptr);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::DeskLink, linked ? "wire" : nullptr));
    row(LINK, LINK_HEARD, nullptr);
    summary(LINK, linked ? "wire" : "box silent", linked ? Level::Good : Level::Bad);
}

void update_bluetooth_link()
{
    char text[ROW_TEXT_SIZE];

    const ble::LinkStats link = ble::link_stats();
    deskproto::Status    proxy{};
    const bool           heard = link.connected && ble::desk::last(proxy);
    const bool           box   = heard && proxy.linked;
    row(LINK, LINK_COMPANION, link.connected ? "connected" : "searching",
        link.connected ? Level::Good : Level::Bad);
    row(LINK, LINK_BOX, !link.connected ? nullptr : box ? "answering" : "silent",
        !link.connected ? Level::Neutral : box ? Level::Good : Level::Bad);
    if (link.samples > 0) {
        std::snprintf(text, sizeof(text), "%d ms", ms_from_us(link.median_us));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::DeskLink, text));
        std::snprintf(text, sizeof(text), "%d ms, worst %d", ms_from_us(link.median_us),
                      ms_from_us(link.max_us));
        row(LINK, LINK_TRIP, text);
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_glance(ui::Glance::DeskLink, link.connected ? nullptr : "searching"));
        row(LINK, LINK_TRIP, nullptr);
    }
    const int quiet = ble::desk::quiet_ms();
    if (quiet >= 0) {
        std::snprintf(text, sizeof(text), "%d.%d s ago", quiet / units::kMsPerSecond,
                      quiet % units::kMsPerSecond / MS_PER_TENTH_SECOND);
        row(LINK, LINK_HEARD, text, quiet < DESK_HEARD_RECENTLY_MS ? Level::Good : Level::Warn);
    } else {
        row(LINK, LINK_HEARD, nullptr);
    }

    if (!link.connected) {
        summary(LINK, "no companion", Level::Bad);
    } else if (!box) {
        summary(LINK, "box silent", Level::Bad);
    } else if (link.samples > 0) {
        std::snprintf(text, sizeof(text), "%d ms", ms_from_us(link.median_us));
        summary(LINK, text, Level::Good);
    } else {
        summary(LINK, "Bluetooth", Level::Good);
    }
}

void update_link()
{
    const bool over_ble = settings::enabled(settings::Key::DeskBluetooth);
    row(LINK, LINK_OVER, over_ble ? "Bluetooth" : "wire");
    if (over_ble) {
        update_bluetooth_link();
    } else {
        update_wire_link();
    }
}

void update_power()
{
    power::State battery{};
    if (!power::last(battery)) {
        summary(POWER, "no monitor", Level::Bad);
        row(POWER, POWER_SOURCE, "no monitor", Level::Bad);
        for (int r : {POWER_CHARGE, POWER_VOLTS, POWER_CURRENT, POWER_STATE}) {
            row(POWER, r, nullptr);
        }
        return;
    }
    row(POWER, POWER_SOURCE, battery.on_battery ? "battery" : "USB");
    if (!battery.present) {
        // Nothing wrong if there is none, so neutral rather than green. A pack
        // run down to its protection looks the same until the charger wakes it.
        summary(POWER, "no battery", Level::Neutral);
        for (int r : {POWER_CHARGE, POWER_CURRENT}) {
            row(POWER, r, nullptr);
        }
        char volts[VALUE_TEXT_SIZE];
        std::snprintf(volts, sizeof(volts), "%.2f V", battery.bus_volts);
        row(POWER, POWER_VOLTS, volts);
        row(POWER, POWER_STATE, "none found");
        return;
    }

    char        text[VALUE_TEXT_SIZE];
    const Level charge = battery.percent >= CHARGE_GOOD_PERCENT  ? Level::Good
                         : battery.percent >= CHARGE_LOW_PERCENT ? Level::Warn
                                                                 : Level::Bad;
    std::snprintf(text, sizeof(text), "%d%%", battery.percent);
    row(POWER, POWER_CHARGE, text, charge);
    summary(POWER, text,
            battery.on_battery && battery.percent < ON_BATTERY_CRITICAL_PERCENT ? Level::Bad
            : battery.on_battery && battery.percent < ON_BATTERY_LOW_PERCENT    ? Level::Warn
                                                                                : Level::Good);
    std::snprintf(text, sizeof(text), "%.2f V", battery.bus_volts);
    row(POWER, POWER_VOLTS, text);
    std::snprintf(text, sizeof(text), "%d mA",
                  static_cast<int>(battery.current_amps * MILLIAMPS_PER_AMP));
    row(POWER, POWER_CURRENT, text);
    row(POWER, POWER_STATE,
        battery.charging     ? "charging"
        : battery.on_battery ? "discharging"
        : battery.full       ? "full"
                             : "idle");
}

void update_radar()
{
    radar::Status shot{};
    radar::status(shot);

    char text[ROW_TEXT_SIZE];
    if (shot.age_s < 0) {
        row(RADAR, RADAR_FEED, nullptr);
        row(RADAR, RADAR_SWEEP, nullptr);
        summary(RADAR, "--", Level::Neutral);
    } else {
        row(RADAR, RADAR_FEED, shot.ok ? "answering" : "refusing",
            shot.ok ? Level::Good : Level::Warn);
        std::snprintf(text, sizeof(text), "%d s ago", shot.age_s);
        row(RADAR, RADAR_SWEEP, text, shot.age_s > RADAR_STALE_S ? Level::Warn : Level::Neutral);
        std::snprintf(text, sizeof(text), "%d plane%s", shot.count, shot.count == 1 ? "" : "s");
        summary(RADAR, text, shot.ok && shot.age_s <= RADAR_STALE_S ? Level::Good : Level::Warn);
    }
    std::snprintf(text, sizeof(text), "%d", shot.count);
    row(RADAR, RADAR_PLANES, text);
    std::snprintf(text, sizeof(text), "%d km", shot.range_km);
    row(RADAR, RADAR_REACH, text);
}

void update_calendar()
{
    static ical::Event ahead[CALENDAR_LOOKAHEAD_EVENTS];
    const int          n = ical::upcoming(ahead, static_cast<int>(std::size(ahead)));

    char text[CALENDAR_TEXT_SIZE];
    std::snprintf(text, sizeof(text), "%d", ical::kFeedCount);
    row(CALENDAR, CAL_FEEDS, text);

    static travel::Option options[travel::kOptionsMax];
    const int             journeys = travel::options(options, travel::kOptionsMax);
    if (journeys > 0) {
        std::snprintf(text, sizeof(text), "%d option%s", journeys, journeys == 1 ? "" : "s");
        row(CALENDAR, CAL_JOURNEY, text, Level::Good);
    } else {
        row(CALENDAR, CAL_JOURNEY, travel::ok() ? "none" : "not asked");
    }

    if (n == 0) {
        row(CALENDAR, CAL_AHEAD, "nothing");
        row(CALENDAR, CAL_NEXT, nullptr);
        summary(CALENDAR, "nothing ahead", Level::Neutral);
        return;
    }
    std::snprintf(text, sizeof(text), "%d event%s", n, n == 1 ? "" : "s");
    row(CALENDAR, CAL_AHEAD, text);

    const auto now     = static_cast<std::int64_t>(std::time(nullptr));
    const int  minutes = static_cast<int>((ahead[0].start - now) / units::kSecondsPerMinute);
    std::tm    local{};
    const auto when = static_cast<std::time_t>(ahead[0].start);
    localtime_r(&when, &local);
    char at[CLOCK_TEXT_SIZE];
    std::snprintf(at, sizeof(at), "%02d:%02d", local.tm_hour, local.tm_min);
    if (minutes > 0) {
        std::snprintf(text, sizeof(text), "%s, in %dh%02dm", at, minutes / units::kMinutesPerHour,
                      minutes % units::kMinutesPerHour);
    } else {
        std::snprintf(text, sizeof(text), "%s, now", at);
    }
    row(CALENDAR, CAL_NEXT, text);
    summary(CALENDAR, at, Level::Good);
}

void uptime_text(char *out, std::size_t size, unsigned seconds)
{
    constexpr unsigned MINUTE = units::kSecondsPerMinute;
    constexpr unsigned HOUR   = units::kSecondsPerHour;
    constexpr unsigned DAY    = units::kSecondsPerDay;
    if (seconds >= DAY) {
        std::snprintf(out, size, "%ud %uh", seconds / DAY, (seconds % DAY) / HOUR);
    } else if (seconds >= HOUR) {
        std::snprintf(out, size, "%uh %um", seconds / HOUR, (seconds % HOUR) / MINUTE);
    } else {
        std::snprintf(out, size, "%um %us", seconds / MINUTE, seconds % MINUTE);
    }
}

void update_system()
{
    const esp_app_desc_t *app = esp_app_get_description();
    row(SYSTEM, SYS_FIRMWARE, app->version);

    char text[SYSTEM_TEXT_SIZE];
    std::snprintf(text, sizeof(text), "%s %s", app->date, app->time);
    row(SYSTEM, SYS_BUILT, text);

    uptime_text(text, sizeof(text),
                static_cast<unsigned>(esp_timer_get_time() / units::kUsPerSecond));
    row(SYSTEM, SYS_UPTIME, text);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::Uptime, text));

    const std::size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const Level       ram      = internal < INTERNAL_RAM_LOW_BYTES     ? Level::Bad
                                 : internal < INTERNAL_RAM_TIGHT_BYTES ? Level::Warn
                                                                       : Level::Good;
    kilobytes(SYSTEM, SYS_INTERNAL, internal, ram);
    std::snprintf(text, sizeof(text), "%u KB free", kib(internal));
    summary(SYSTEM, text, ram);
    // What a client restart needs in one piece for its task stack.
    kilobytes(SYSTEM, SYS_LARGEST, heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    kilobytes(SYSTEM, SYS_PSRAM, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    kilobytes(SYSTEM, SYS_LOW, heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

    const media::Status decoder = media::status();
    if (decoder.decodes == 0) {
        row(SYSTEM, SYS_JPEG, "not used yet");
    } else {
        std::snprintf(text, sizeof(text), "%s, %d ms", decoder.hardware ? "engine" : "software",
                      decoder.decode_ms);
        row(SYSTEM, SYS_JPEG, text);
    }
}

// ---- Which card keeps which log lines. ----

constexpr const char *WIFI_TAGS[] = {
    "wifi",       "esp_netif_handlers", "esp_netif",  "esp_wifi_remote", "wifi_init",
    "H_API",      "H_SDIO_DRV",         "transport",  "sdio_wrapper",    "esp_hosted",
    "net"};
constexpr const char *HASS_TAGS[] = {"hass",        "ha_ws",       "websocket_client", "room",
                                     "jellyfin",
                                     "network",     "mqtt_client", "MQTT_CLIENT",
                                     "transport_base", "media"};
constexpr const char *PRESENCE_TAGS[] = {"presence", "ble",      "NimBLE",
                                         "vhci_drv", "BTDM_INIT", "phy_init"};
constexpr const char *DESK_TAGS[]     = {"desk", "loctek"};
constexpr const char *LINK_TAGS[]     = {"desklink", "deskproxy", "proxy", "leds", "update"};
constexpr const char *POWER_TAGS[]    = {"power", "battery"};
constexpr const char *RADAR_TAGS[]    = {"radar"};
constexpr const char *CALENDAR_TAGS[] = {"ical", "travel"};
constexpr const char *SYSTEM_TAGS[]   = {
    "panel", "ui",     "shot",   "diag",      "clock",     "settings", "logbuf",   "board",
    "rtc",   "main_task", "cpu_start", "heap_init", "spiram", "esp_psram", "esp_image",
    "jpeg",  "sound",  "imu",    "BMI270",    "orientation", "focus", "ota",       "remote",   "jobs"};

struct TagSet {
    Card               card;
    const char *const *tags;
    int                count;
};

constexpr TagSet TAG_SETS[] = {
    {WIFI, WIFI_TAGS, static_cast<int>(std::size(WIFI_TAGS))},
    {HASS, HASS_TAGS, static_cast<int>(std::size(HASS_TAGS))},
    {PRESENCE, PRESENCE_TAGS, static_cast<int>(std::size(PRESENCE_TAGS))},
    {DESK, DESK_TAGS, static_cast<int>(std::size(DESK_TAGS))},
    {LINK, LINK_TAGS, static_cast<int>(std::size(LINK_TAGS))},
    {POWER, POWER_TAGS, static_cast<int>(std::size(POWER_TAGS))},
    {RADAR, RADAR_TAGS, static_cast<int>(std::size(RADAR_TAGS))},
    {CALENDAR, CALENDAR_TAGS, static_cast<int>(std::size(CALENDAR_TAGS))},
    {SYSTEM, SYSTEM_TAGS, static_cast<int>(std::size(SYSTEM_TAGS))},
};
static_assert(std::size(TAG_SETS) == CARDS, "a tag set per card");

// Each a moment of blue screen, which otherwise leaves no trace in the log.
void note_underruns()
{
    static std::uint32_t told = 0;
    const std::uint32_t  now  = board::underruns();
    if (now != told) {
        ESP_LOGW("board", "the panel's reads fell behind %u times, %u since the last",
                 static_cast<unsigned>(now), static_cast<unsigned>(now - told));
        told = now;
    }

    // Seven frames: the panel's DMA goes round eight by itself, one of them
    // perhaps half gone when its interrupt is held off.
    constexpr std::int32_t  WENT_WITHOUT_US = 120000;
    static std::uint32_t    late_told       = 0;
    const board::LateFrames late            = board::late_frames();
    if (late.count != late_told) {
        const int ago_ms = static_cast<int>((esp_timer_get_time() - late.at_us) / 1000);
        const int gap_ms = static_cast<int>(late.gap_us / 1000);
        if (late.gap_us > WENT_WITHOUT_US) {
            ESP_LOGW("board", "the panel went without frames: its interrupt held off %d ms, %d ms ago", gap_ms, ago_ms);
        } else {
            ESP_LOGI("board", "the panel's interrupt held off %d ms, %d ms ago; %u in all", gap_ms, ago_ms,
                     static_cast<unsigned>(late.count));
        }
        late_told = late.count;
    }
}

void update()
{
    update_wifi();
    update_hass();
    update_presence();
    update_desk();
    update_link();
    update_power();
    update_radar();
    update_calendar();
    update_system();
}

jobs::Result tick()
{
    note_underruns();
    if (ui::diagnostics_open()) {
        update();
    }
    return jobs::done();
}

}  // namespace

const ui::Card *cards()
{
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        DESK_ROWS[DESK_PRESET + i] = ui::preset_name(i);
    }
    return CARDS_TABLE;
}

int card_count()
{
    return CARDS;
}

esp_err_t start()
{
    jobs::Spec spec;
    spec.name      = TAG;
    spec.period_ms = TICK_MS;
    spec.run       = tick;
    s_job          = jobs::add(std::move(spec));
    ESP_RETURN_ON_FALSE(s_job != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

void refresh()
{
    jobs::poke(s_job);
}

int route(const char *tag)
{
    for (const TagSet &set : TAG_SETS) {
        for (int i = 0; i < set.count; ++i) {
            if (std::strcmp(tag, set.tags[i]) == 0) {
                return set.card;
            }
        }
    }
    // Anything unrecognised is still worth keeping, and System is where somebody
    // would go looking for it.
    return SYSTEM;
}

int channels()
{
    return CARDS;
}

int logs(int card, bool warnings, ui::LogLine *out, int max)
{
    // One set of entries, kept rather than asked for every second.
    constexpr int         ENTRIES = 200;
    static logbuf::Entry *entries = static_cast<logbuf::Entry *>(
        heap_caps_malloc(sizeof(logbuf::Entry) * ENTRIES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (entries == nullptr) {
        return 0;
    }
    const std::uint32_t mask  = card < 0 ? ~0u : 1u << card;
    const int           count = logbuf::recent(mask, warnings, entries, std::min(max, ENTRIES));
    for (int i = 0; i < count; ++i) {
        out[i].level = entries[i].level;
        out[i].card  = entries[i].channel;
        std::snprintf(out[i].text, sizeof(out[i].text), "%s", entries[i].text);
    }
    return count;
}

}  // namespace diagnostics
