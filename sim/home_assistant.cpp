#include "home_assistant.h"

#include "hardware.h"
#include "room_layout.h"

#include <cstdio>
#include <iterator>

namespace home_assistant {
namespace {
constexpr float MIN_C = 15.0f, MAX_C = 25.0f, STEP_C = 0.5f;

bool  s_up                         = false;
bool  s_light[room::LIGHT_COUNT]   = {true, false, false, true};
bool  s_toggle[room::TOGGLE_COUNT] = {};
float s_current_c                  = 20.5f;
float s_target_c                   = 21.0f;
bool  s_heating                    = true;  // the mode, heat or off
int   s_air                        = 0;

// Each reading's value in each of the three airs, in the order room_layout has them.
constexpr float AIRS[][room::PILL_COUNT] = {
    {640, 120, 48, 4},
    {950, 410, 62, 18},
    {1450, 1300, 72, 40},
};
constexpr const char *UNITS[room::PILL_COUNT] = {"ppm", "ppb", "%", "µg/m³"};

ui::Level level_of(const room::PillSpec &spec, float value)
{
    if (value >= spec.good_lo && value <= spec.good_hi) {
        return ui::Level::Good;
    }
    return value >= spec.warn_lo && value <= spec.warn_hi ? ui::Level::Warn : ui::Level::Bad;
}

void show()
{
    if (!s_up) {
        room::show_unknown();
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat(-1.0f, -1.0f, "--", ui::Hvac::Off));
        return;
    }
    for (int i = 0; i < room::PILL_COUNT; ++i) {
        char        text[24];
        const float value = AIRS[s_air][i];
        std::snprintf(text, sizeof(text), "%g %s", value, UNITS[i]);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pill(i, room::PILLS[i].label, text, level_of(room::PILLS[i], value)));
    }
    int on = 0;
    for (int i = 0; i < room::LIGHT_COUNT; ++i) {
        on += s_light[i] ? 1 : 0;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, room::LIGHTS[i].name, s_light[i] ? "On" : "Off", s_light[i]));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_lights("LIGHTS", on > 0 ? "On" : "Off", on > 0));
    for (int i = 0; i < room::TOGGLE_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_dial_toggle(i, room::toggle_label(room::TOGGLES[i], s_toggle[i]), s_toggle[i]));
    }
    const ui::Hvac hvac = !s_heating ? ui::Hvac::Off : s_current_c < s_target_c ? ui::Hvac::Heating : ui::Hvac::Idle;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat(s_current_c, s_target_c, s_heating ? "HEAT" : "OFF", hvac));
}
}  // namespace

void start()
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat_range(MIN_C, MAX_C, STEP_C));
    show();
}

void toggle()
{
    s_up = !s_up;
    hardware::set_home_assistant(s_up);
    show();
}

void next_air()
{
    s_air = (s_air + 1) % static_cast<int>(std::size(AIRS));
    show();
}

void on_lights()
{
    if (!s_up) {
        return;
    }
    bool any = false;
    for (bool on : s_light) {
        any = any || on;
    }
    for (bool &on : s_light) {
        on = !any;  // the script turns them all off, or all on
    }
    show();
}

void on_light(int index)
{
    if (s_up && index >= 0 && index < room::LIGHT_COUNT) {
        s_light[index] = !s_light[index];
        show();
    }
}

void on_setpoint(float celsius)
{
    if (s_up) {
        s_target_c = celsius;
        show();
    }
}

void on_mode()
{
    if (s_up) {
        s_heating = !s_heating;
        show();
    }
}

void on_dial_toggle(int index)
{
    if (s_up && index >= 0 && index < room::TOGGLE_COUNT) {
        s_toggle[index] = !s_toggle[index];
        show();
    }
}
}  // namespace home_assistant
