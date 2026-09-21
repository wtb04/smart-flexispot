#include "room.h"

#include "esp_log.h"
#include "ha_ws.h"
#include "media.h"
#include "ui.h"

#include <cstdio>
#include <atomic>
#include <ctime>
#include <cctype>
#include <cstring>
#include <string>

namespace room {
namespace {

constexpr char TAG[] = "room";

constexpr char CLIMATE_ENTITY[] = "climate.office_thermostaat";

struct PillSpec {
    const char *entity;
    const char *label;
    float       good_lo, good_hi;
    float       warn_lo, warn_hi;
};

constexpr PillSpec PILLS[] = {
    {"sensor.office_awair_carbon_dioxide", "CO2", 0.0f, 800.0f, 0.0f, 1200.0f},
    // Awair's own bands: comfortable below 333 ppb, worth a window above 1000.
    {"sensor.office_awair_volatile_organic_compounds_parts", "VOC", 0.0f, 333.0f, 0.0f,
     1000.0f},
    {"sensor.office_awair_humidity", "HUMIDITY", 40.0f, 60.0f, 30.0f, 70.0f},
    {"sensor.office_awair_pm2_5", "PM2.5", 0.0f, 12.0f, 0.0f, 35.0f},
};

struct LightSpec {
    const char *entity;
    const char *name;
};

constexpr LightSpec LIGHTS[] = {
    {"light.office_bureaulamp", "Desk lamp"},
    {"light.office_lamp_muur", "Wall lamp"},
    {"light.office_bed", "Bed"},
    {"light.office_grote_lamp", "Main lamp"},
};

// Two scripts rather than a toggle; the state helper says which one runs.
constexpr char ALL_LIGHTS_ENTITY[] = "input_boolean.office_verlichting_actief";
constexpr char ALL_LIGHTS_ON[]     = "script.office_verlichting_aan";
constexpr char ALL_LIGHTS_OFF[]    = "script.office_verlichting_uit";

// The chip reads as the position it is in; the colour says whether it is live.
struct ToggleSpec {
    const char *entity;
    const char *on_label;
    const char *off_label;
};

constexpr ToggleSpec TOGGLES[] = {
    {"input_boolean.office_alleen_kast", "1", "2"},
};

const char *toggle_label(const ToggleSpec &spec, bool on)
{
    return on ? spec.on_label : spec.off_label;
}

constexpr char MEDIA_ENTITY[] = "media_player.office_speaker";

/** Fraction of full scale, per press. */
constexpr float VOLUME_STEP = 0.05f;

constexpr int PILL_COUNT   = sizeof(PILLS) / sizeof(PILLS[0]);
constexpr int LIGHT_COUNT  = sizeof(LIGHTS) / sizeof(LIGHTS[0]);
constexpr int TOGGLE_COUNT = sizeof(TOGGLES) / sizeof(TOGGLES[0]);

static_assert(PILL_COUNT <= ui::kPillCount, "more readings than the strip has chips");
static_assert(LIGHT_COUNT <= ui::kLightCount, "more lights than the picker has buttons");
static_assert(TOGGLE_COUNT <= ui::kDialToggleCount, "more toggles than the dial has corners");

bool is_on(const std::string &state)
{
    return state == "on" || state == "playing" || state == "heat" || state == "cool";
}

bool known(const hass::ws::Entity *entity)
{
    return entity != nullptr && entity->state != "unavailable" && entity->state != "unknown";
}

std::string display_value(const hass::ws::Entity &entity)
{
    if (!known(&entity)) {
        return "--";
    }
    if (!entity.unit.empty()) {
        return entity.state + " " + entity.unit;
    }
    return entity.state;
}

const char *on_off(const hass::ws::Entity *entity)
{
    if (!known(entity)) {
        return "--";
    }
    return is_on(entity->state) ? "ON" : "OFF";
}

// What was last handed to the progress bar. Home Assistant reports
// media_position only when it changes, so re-pushing the same stale number on
// every event dragged the bar backwards.
std::string s_position_stamp;
int         s_position_duration = -1;
bool        s_position_playing  = false;

/** Days from 1970-01-01, by Howard Hinnant's civil calendar. */
long long days_from_civil(const std::tm &date)
{
    long long       year  = date.tm_year + 1900;
    const unsigned  month = static_cast<unsigned>(date.tm_mon) + 1;
    const unsigned  day   = static_cast<unsigned>(date.tm_mday);
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned  yoe = static_cast<unsigned>(year - era * 400);
    const unsigned  doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

/** Seconds since a Home Assistant timestamp, or 0 if it cannot be read. */
int seconds_since(const std::string &iso)
{
    std::tm parsed{};
    if (iso.size() < 19 || strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &parsed) == nullptr) {
        return 0;
    }
    // timegm is not in newlib's headers here, and mktime would apply the panel's
    // timezone to a stamp Home Assistant already sends in UTC.
    const std::time_t when = days_from_civil(parsed) * 86400LL + parsed.tm_hour * 3600LL +
                             parsed.tm_min * 60LL + parsed.tm_sec;
    const std::time_t now  = std::time(nullptr);
    // Before SNTP lands the clock is at the epoch, which reads as a wildly
    // negative age; anything implausible means no information.
    const double age = std::difftime(now, when);
    return age > 0.0 && age < 24 * 3600 ? static_cast<int>(age) : 0;
}

// render() runs on the WebSocket task and the handlers on the LVGL task. Letting
// a handler read the entity store meant walking a std::map while the other task
// rebalanced it -- a use-after-free waiting for a press to coincide with an
// update. Atomics cross the boundary instead: one read per handler, no tearing.
std::atomic<bool> s_climate_on{false};
std::atomic<bool> s_all_lights_on{false};
std::atomic<bool> s_light_on[LIGHT_COUNT];
std::atomic<bool> s_toggle_on[TOGGLE_COUNT];
std::atomic<bool> s_muted{false};
std::atomic<int>  s_volume_pct{-1};  // negative until the speaker reports one

std::string attribute(const hass::ws::Entity &entity, const char *key)
{
    const auto it = entity.attributes.find(key);
    return it == entity.attributes.end() ? std::string{} : it->second;
}

/** -1 when the attribute is absent or not a number. */
float attribute_number(const hass::ws::Entity &entity, const char *key)
{
    const auto it = entity.attributes.find(key);
    if (it == entity.attributes.end()) {
        return -1.0f;
    }
    char       *end   = nullptr;
    const float value = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? -1.0f : value;
}

ui::Level pill_level(const PillSpec &spec, const hass::ws::Entity *entity)
{
    if (!known(entity)) {
        return ui::Level::Neutral;
    }
    char       *end   = nullptr;
    const float value = std::strtof(entity->state.c_str(), &end);
    if (end == entity->state.c_str()) {
        return ui::Level::Neutral;
    }
    if (value >= spec.good_lo && value <= spec.good_hi) {
        return ui::Level::Good;
    }
    return value >= spec.warn_lo && value <= spec.warn_hi ? ui::Level::Warn : ui::Level::Bad;
}

std::string upper(std::string text)
{
    for (char &c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

/** "script.foo" is called as domain "script", service "foo". */
void run_script(const char *script)
{
    const std::string full{script};
    const std::size_t dot = full.find('.');
    if (dot == std::string::npos) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service("script", full.substr(dot + 1).c_str(), script));
}

void render_thermostat(const hass::ws::EntityStore &store)
{
    const hass::ws::Entity *climate = store.find(CLIMATE_ENTITY);
    if (climate == nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat(-1.0f, -1.0f, "--", ui::Hvac::Off));
        return;
    }

    const float min_c  = attribute_number(*climate, "min_temp");
    const float max_c  = attribute_number(*climate, "max_temp");
    const float step_c = attribute_number(*climate, "target_temp_step");
    if (min_c > 0.0f && max_c > min_c) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat_range(min_c, max_c, step_c));
    }

    // hvac_action says whether the boiler is running; the state only says what it
    // has been asked to do.
    const auto action  = climate->attributes.find("hvac_action");
    const bool heating = action != climate->attributes.end() ? action->second == "heating"
                                                             : is_on(climate->state);
    const ui::Hvac state = climate->state == "off" ? ui::Hvac::Off
                                                   : (heating ? ui::Hvac::Heating : ui::Hvac::Idle);
    s_climate_on.store(state != ui::Hvac::Off, std::memory_order_relaxed);

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_thermostat(attribute_number(*climate, "current_temperature"),
                           attribute_number(*climate, "temperature"),
                           upper(climate->state).c_str(), state));
}

void render_media(const hass::ws::EntityStore &store)
{
    const hass::ws::Entity *player = store.find(MEDIA_ENTITY);
    if (player == nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("SPEAKER", "", "", "--", false));
        media::set_art_path("");
        return;
    }

    const bool        playing = player->state == "playing";
    const std::string title   = attribute(*player, "media_title");
    const std::string artist  = attribute(*player, "media_artist");
    const std::string source  = upper(attribute(*player, "app_name"));
    const std::string state   = upper(player->state);

    s_muted.store(attribute(*player, "is_volume_muted") == "true", std::memory_order_relaxed);

    // set_media is the one setter that is not idempotent -- it rewrites the
    // cover's recolour and re-runs the text layout -- so pushing it on every
    // entity event repainted the album art because a CO2 sensor reported.
    static std::string s_shown;
    const std::string  shown = source + '\n' + title + '\n' + artist + '\n' + state +
                              (playing ? "\n1" : "\n0");
    if (shown != s_shown) {
        s_shown = shown;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media(source.c_str(), title.c_str(), artist.c_str(),
                                                    state.c_str(), playing));
    }

    const std::string stamp    = attribute(*player, "media_position_updated_at");
    const int         duration = static_cast<int>(attribute_number(*player, "media_duration"));
    if (stamp != s_position_stamp || duration != s_position_duration ||
        playing != s_position_playing) {
        s_position_stamp    = stamp;
        s_position_duration = duration;
        s_position_playing  = playing;

        // Carried forward to now: the reading was only true when it was made.
        const int reported = static_cast<int>(attribute_number(*player, "media_position"));
        const int elapsed  = reported + (playing ? seconds_since(stamp) : 0);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_progress(elapsed, duration, playing));
    }

    const float level = attribute_number(*player, "volume_level");
    if (level >= 0.0f) {
        const int percent = static_cast<int>(level * 100.0f + 0.5f);
        s_volume_pct.store(percent, std::memory_order_relaxed);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(percent));
    }

    // The local proxy rather than entity_picture: Home Assistant serves that over
    // plain HTTP with a token in the path.
    media::set_art_path(title.empty() ? ""
                                      : attribute(*player, "entity_picture_local").c_str());
}

}  // namespace

// The volume_up and volume_down services step by whatever the player thinks is
// sensible, a tenth of full scale here -- too coarse to settle on a level.
void nudge_volume(float delta)
{
    const int reported = s_volume_pct.load(std::memory_order_relaxed);
    if (reported < 0) {
        return;  // a blind guess would jump the volume
    }
    float wanted = static_cast<float>(reported) / 100.0f + delta;
    wanted       = wanted < 0.0f ? 0.0f : (wanted > 1.0f ? 1.0f : wanted);

    const int percent = static_cast<int>(wanted * 100.0f + 0.5f);
    ESP_LOGI(TAG, "volume %d%%", percent);

    // Shown before the round trip, like every other control here: if the call is
    // refused, the next update corrects it within a moment.
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(percent));

    char value[16];
    std::snprintf(value, sizeof(value), "%.2f", static_cast<double>(wanted));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service_with("media_player", "volume_set",
                                                              MEDIA_ENTITY, "volume_level", value));
}

void on_media(ui::MediaAction action)
{
    switch (action) {
        case ui::MediaAction::PlayPause:
            hass::ws::call_service("media_player", "media_play_pause", MEDIA_ENTITY);
            break;
        case ui::MediaAction::Previous:
            hass::ws::call_service("media_player", "media_previous_track", MEDIA_ENTITY);
            break;
        case ui::MediaAction::Next:
            hass::ws::call_service("media_player", "media_next_track", MEDIA_ENTITY);
            break;
        case ui::MediaAction::VolumeDown:
            nudge_volume(-VOLUME_STEP);
            break;
        case ui::MediaAction::VolumeUp:
            nudge_volume(VOLUME_STEP);
            break;
        case ui::MediaAction::Mute: {
            const bool muted = s_muted.load(std::memory_order_relaxed);
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                hass::ws::call_service_with("media_player", "volume_mute", MEDIA_ENTITY,
                                            "is_volume_muted", muted ? "false" : "true"));
            break;
        }
    }
}

void init()
{
    for (int i = 0; i < PILL_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_pill(i, PILLS[i].label, "--", ui::Level::Neutral));
    }
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_dial_toggle(i, toggle_label(TOGGLES[i], false), false));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_lights("LIGHTS", "--", false));
    for (int i = 0; i < LIGHT_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, LIGHTS[i].name, "--", false));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("SPEAKER", "", "", "--", false));
}

void render(const hass::ws::EntityStore &store)
{
    render_thermostat(store);

    for (int i = 0; i < PILL_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(PILLS[i].entity);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_pill(i, PILLS[i].label,
                         entity != nullptr ? display_value(*entity).c_str() : "--",
                         pill_level(PILLS[i], entity)));
    }
    for (int i = PILL_COUNT; i < ui::kPillCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pill(i, "", "", ui::Level::Neutral));
    }

    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(TOGGLES[i].entity);
        const bool              on     = entity != nullptr && is_on(entity->state);
        s_toggle_on[i].store(on, std::memory_order_relaxed);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_dial_toggle(i, toggle_label(TOGGLES[i], on), on));
    }
    for (int i = TOGGLE_COUNT; i < ui::kDialToggleCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_dial_toggle(i, "", false));
    }

    const hass::ws::Entity *all = store.find(ALL_LIGHTS_ENTITY);
    s_all_lights_on.store(all != nullptr && is_on(all->state), std::memory_order_relaxed);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_lights("LIGHTS", on_off(all), all != nullptr && is_on(all->state)));

    for (int i = 0; i < LIGHT_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(LIGHTS[i].entity);
        const bool              on     = entity != nullptr && is_on(entity->state);
        s_light_on[i].store(on, std::memory_order_relaxed);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, LIGHTS[i].name, on_off(entity), on));
    }
    for (int i = LIGHT_COUNT; i < ui::kLightCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, "", "", false));
    }

    render_media(store);
}

void on_setpoint(float celsius)
{
    ESP_LOGI(TAG, "setpoint %d.%d C", static_cast<int>(celsius),
             static_cast<int>(celsius * 10) % 10);
    char value[16];
    std::snprintf(value, sizeof(value), "%.1f", celsius);
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service_with("climate", "set_temperature",
                                                              CLIMATE_ENTITY, "temperature",
                                                              value));
}

void on_mode()
{
    const bool  currently_on = s_climate_on.load(std::memory_order_relaxed);
    const char *mode         = currently_on ? "off" : "heat";
    ESP_LOGI(TAG, "hvac mode -> %s", mode);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service_with("climate", "set_hvac_mode", CLIMATE_ENTITY, "hvac_mode", mode));
}

void on_lights()
{
    const bool currently_on = s_all_lights_on.load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "all lights -> %s", currently_on ? "off" : "on");

    // Flipped straight away; Home Assistant's next update overwrites it.
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_lights("LIGHTS", currently_on ? "OFF" : "ON", !currently_on));
    run_script(currently_on ? ALL_LIGHTS_OFF : ALL_LIGHTS_ON);
}

void on_light(int index)
{
    if (index < 0 || index >= LIGHT_COUNT) {
        return;
    }
    const bool currently_on = s_light_on[index].load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "light %s -> %s", LIGHTS[index].name, currently_on ? "off" : "on");

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(index, LIGHTS[index].name,
                                                currently_on ? "OFF" : "ON", !currently_on));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service("light", "toggle", LIGHTS[index].entity));
}

void on_dial_toggle(int index)
{
    if (index < 0 || index >= TOGGLE_COUNT) {
        return;
    }
    const bool currently_on = s_toggle_on[index].load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "toggle %s -> %s", TOGGLES[index].entity, currently_on ? "off" : "on");

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_dial_toggle(index, toggle_label(TOGGLES[index], !currently_on), !currently_on));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service("input_boolean", "toggle", TOGGLES[index].entity));
}

}  // namespace room
