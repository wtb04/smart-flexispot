#include "room.h"

#include "esp_log.h"
#include "ha_ws.h"
#include "players.h"
#include "radar.h"
#include "room_layout.h"
#include "ui.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace room {
namespace {
constexpr char TAG[] = "room";
constexpr std::size_t SERVICE_VALUE_SIZE = 16;
constexpr int         TENTHS_PER_DEGREE  = 10;

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

std::atomic<bool> s_climate_on{false};
std::atomic<bool> s_all_lights_on{false};
std::atomic<bool> s_light_on[LIGHT_COUNT];
std::atomic<bool> s_toggle_on[TOGGLE_COUNT];
std::atomic<int> s_entity_count{0};

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

}  // namespace

void init()
{
    media_init();
    show_unknown();
}

void render(const hass::ws::EntityStore &store)
{
    s_entity_count.store(static_cast<int>(store.size()), std::memory_order_relaxed);

    if (const hass::ws::Entity *home = store.find("zone.home"); home != nullptr) {
        const float lat = attribute_number(*home, "latitude");
        const float lon = attribute_number(*home, "longitude");
        if (lat != kNoNumber && lon != kNoNumber) {
            radar::set_home(lat, lon);
        }
    }

    render_thermostat(store);
    start_picks();

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
    if (const hass::ws::Entity *main = store.find(MAIN_LIGHT_ENTITY); all != nullptr || main != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_room_lit((all != nullptr && is_on(all->state)) ||
                                                       (main != nullptr && is_on(main->state))));
    }

    for (int i = 0; i < LIGHT_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(LIGHTS[i].entity);
        const bool              on     = entity != nullptr && is_on(entity->state);
        s_light_on[i].store(on, std::memory_order_relaxed);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, LIGHTS[i].name, on_off(entity), on));
    }
    for (int i = LIGHT_COUNT; i < ui::kLightCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, "", "", false));
    }

    media_from_store(store);
}

void on_setpoint(float celsius)
{
    ESP_LOGI(TAG, "setpoint %d.%d C", static_cast<int>(celsius),
             static_cast<int>(celsius * TENTHS_PER_DEGREE) % TENTHS_PER_DEGREE);
    char value[SERVICE_VALUE_SIZE];
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

std::vector<std::string> entities()
{
    std::vector<std::string> out = {CLIMATE_ENTITY, ALL_LIGHTS_ENTITY, ALL_LIGHTS_ON,
                                    ALL_LIGHTS_OFF, MEDIA_SPEAKER,
                                    "zone.home"};  // the radar's centre
    for (const PillSpec &pill : PILLS) {
        out.emplace_back(pill.entity);
    }
    for (const LightSpec &light : LIGHTS) {
        out.emplace_back(light.entity);
    }
    for (const ToggleSpec &toggle : TOGGLES) {
        out.emplace_back(toggle.entity);
    }
    return out;
}

std::vector<std::string> attributes()
{
    return {"min_temp",          "max_temp",           "target_temp_step",
            "hvac_action",       "current_temperature", "temperature",
            "media_title",       "media_artist",        "media_series_title",
            "media_season",      "media_episode",       "app_name",
            "is_volume_muted",   "media_position_updated_at",
            "media_duration",    "media_position",      "volume_level",
            "entity_picture_local", "entity_picture",   "latitude",
            "longitude",         "supported_features"};
}

int entity_count()
{
    return s_entity_count.load(std::memory_order_relaxed);
}

}  // namespace room
