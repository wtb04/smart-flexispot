#include "room.h"

#include "esp_log.h"
#include "ha_ws.h"
#include "ui.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>

namespace room {
namespace {

constexpr char TAG[] = "room";

constexpr char CLIMATE_ENTITY[] = "climate.office_thermostaat";

// Readings along the top: glanced at, never pressed. The bands turn a number
// nobody remembers the healthy range for into a colour anyone can read from
// across the room; a band that covers everything means no opinion.
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

// The individual lights behind the lights button's long press.
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

// All the lamps at once. Two scripts rather than a toggle, so which one runs
// depends on where the state helper currently sits.
constexpr char ALL_LIGHTS_ENTITY[] = "input_boolean.office_verlichting_actief";
constexpr char ALL_LIGHTS_ON[]     = "script.office_verlichting_aan";
constexpr char ALL_LIGHTS_OFF[]    = "script.office_verlichting_uit";

// Corner toggles on the thermostat card. A two-position switch: the chip reads
// as the position it is in, and the colour says whether that position is live.
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

// Whatever is left over goes in the row under the lights button.
struct TileSpec {
    const char *entity;
    const char *label;
    const char *domain;  // nullptr for a readout
};

constexpr TileSpec TILES[] = {
    {"media_player.office_speaker", "Speaker", "media_player"},
};

constexpr int PILL_COUNT   = sizeof(PILLS) / sizeof(PILLS[0]);
constexpr int LIGHT_COUNT  = sizeof(LIGHTS) / sizeof(LIGHTS[0]);
constexpr int TOGGLE_COUNT = sizeof(TOGGLES) / sizeof(TOGGLES[0]);
constexpr int TILE_COUNT   = sizeof(TILES) / sizeof(TILES[0]);

static_assert(PILL_COUNT <= ui::kPillCount, "more readings than the strip has chips");
static_assert(LIGHT_COUNT <= ui::kLightCount, "more lights than the picker has buttons");
static_assert(TOGGLE_COUNT <= ui::kDialToggleCount, "more toggles than the dial has corners");
static_assert(TILE_COUNT <= ui::kTileCount, "more entities than the row has slots");

/** True for the states Home Assistant considers active. */
bool is_on(const std::string &state)
{
    return state == "on" || state == "playing" || state == "heat" || state == "cool";
}

bool known(const hass::ws::Entity *entity)
{
    return entity != nullptr && entity->state != "unavailable" && entity->state != "unknown";
}

/** What to show in the large text: a reading with its unit, or a state. */
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

// The tap handlers run from the LVGL task and need the current state to pick a
// direction, so the last store seen is kept here.
const hass::ws::EntityStore *s_store = nullptr;

const hass::ws::Entity *find(const char *entity_id)
{
    return s_store != nullptr ? s_store->find(entity_id) : nullptr;
}

/** Reads a numeric attribute, or -1 when it is absent or not a number. */
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

/** Where a reading sits in its spec's bands, or Neutral if it is not a number. */
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

    // Bounds and step belong to the thermostat, not to us.
    const float min_c  = attribute_number(*climate, "min_temp");
    const float max_c  = attribute_number(*climate, "max_temp");
    const float step_c = attribute_number(*climate, "target_temp_step");
    if (min_c > 0.0f && max_c > min_c) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat_range(min_c, max_c, step_c));
    }

    // hvac_action says whether the boiler is running; the state only says what
    // it has been asked to do.
    const auto action  = climate->attributes.find("hvac_action");
    const bool heating = action != climate->attributes.end() ? action->second == "heating"
                                                             : is_on(climate->state);
    const ui::Hvac state = climate->state == "off" ? ui::Hvac::Off
                                                   : (heating ? ui::Hvac::Heating : ui::Hvac::Idle);

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_thermostat(attribute_number(*climate, "current_temperature"),
                           attribute_number(*climate, "temperature"),
                           upper(climate->state).c_str(), state));
}

}  // namespace

void init()
{
    // The same slots the first update will fill, so the screen is laid out and
    // legible from boot rather than assembling itself once the socket comes up.
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
    for (int i = 0; i < TILE_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_tile(i, TILES[i].label, "--", false, false));
    }
}

void render(const hass::ws::EntityStore &store)
{
    s_store = &store;
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
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_dial_toggle(i, toggle_label(TOGGLES[i], on), on));
    }
    for (int i = TOGGLE_COUNT; i < ui::kDialToggleCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_dial_toggle(i, "", false));
    }

    const hass::ws::Entity *all = store.find(ALL_LIGHTS_ENTITY);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_lights("LIGHTS", on_off(all), all != nullptr && is_on(all->state)));

    for (int i = 0; i < LIGHT_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(LIGHTS[i].entity);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, LIGHTS[i].name, on_off(entity),
                                                    entity != nullptr && is_on(entity->state)));
    }
    for (int i = LIGHT_COUNT; i < ui::kLightCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, "", "", false));
    }

    for (int i = 0; i < TILE_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(TILES[i].entity);
        // Not reported yet: keep the slot so the layout is stable, but say
        // nothing rather than implying a state it might not have.
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_tile(i, TILES[i].label,
                         entity != nullptr ? display_value(*entity).c_str() : "...",
                         entity != nullptr && is_on(entity->state),
                         TILES[i].domain != nullptr));
    }
    for (int i = TILE_COUNT; i < ui::kTileCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_tile(i, "", "", false, false));
    }
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
    const hass::ws::Entity *climate      = find(CLIMATE_ENTITY);
    const bool              currently_on = climate != nullptr && climate->state != "off";
    const char             *mode         = currently_on ? "off" : "heat";
    ESP_LOGI(TAG, "hvac mode -> %s", mode);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service_with("climate", "set_hvac_mode", CLIMATE_ENTITY, "hvac_mode", mode));
}

void on_lights()
{
    const hass::ws::Entity *state        = find(ALL_LIGHTS_ENTITY);
    const bool              currently_on = state != nullptr && is_on(state->state);
    ESP_LOGI(TAG, "all lights -> %s", currently_on ? "off" : "on");

    // Flipped straight away rather than after the round trip. Home Assistant's
    // next update overwrites this, so a command that fails corrects itself
    // within a moment instead of lying.
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_lights("LIGHTS", currently_on ? "OFF" : "ON", !currently_on));
    run_script(currently_on ? ALL_LIGHTS_OFF : ALL_LIGHTS_ON);
}

void on_light(int index)
{
    if (index < 0 || index >= LIGHT_COUNT) {
        return;
    }
    const hass::ws::Entity *entity       = find(LIGHTS[index].entity);
    const bool              currently_on = entity != nullptr && is_on(entity->state);
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
    const hass::ws::Entity *entity       = find(TOGGLES[index].entity);
    const bool              currently_on = entity != nullptr && is_on(entity->state);
    ESP_LOGI(TAG, "toggle %s -> %s", TOGGLES[index].entity, currently_on ? "off" : "on");

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_dial_toggle(index, toggle_label(TOGGLES[index], !currently_on), !currently_on));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service("input_boolean", "toggle", TOGGLES[index].entity));
}

void on_tile(int index)
{
    if (index < 0 || index >= TILE_COUNT || TILES[index].domain == nullptr) {
        return;
    }
    const hass::ws::Entity *entity       = find(TILES[index].entity);
    const bool              currently_on = entity != nullptr && is_on(entity->state);
    ESP_LOGI(TAG, "tile %s -> %s", TILES[index].label, currently_on ? "off" : "on");

    if (entity != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_tile(index, TILES[index].label,
                                                   display_value(*entity).c_str(), !currently_on,
                                                   true));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service(TILES[index].domain, "toggle", TILES[index].entity));
}

}  // namespace room
