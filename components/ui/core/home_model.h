#pragma once

#include "ui.h"

#include <cstdint>

// The home page's readings and controls as Home Assistant last gave them: the
// pills along the top, the thermostat and the toggles in its corners. What
// applies the updates writes them and publishes Topic::Home. On the LVGL task only.
namespace ui::detail {

struct PillState {
    char  label[40] = "";  // empty leaves the pill out
    char  value[40] = "";
    Level level     = Level::Neutral;
};

struct ToggleState {
    char label[32] = "";  // empty leaves it out
    bool on        = false;
};

struct ThermostatState {
    bool  known     = false;  // told at least once
    float current_c = -1.0f;
    float target_c  = -1.0f;
    char  mode[24]  = "";
    Hvac  state     = Hvac::Off;
    bool  ranged    = false;  // its range told
    float min_c = 0.0f, max_c = 0.0f, step_c = 0.0f;
};

struct HomeState {
    PillState       pills[kPillCount];
    ToggleState     toggles[kDialToggleCount];
    ThermostatState thermostat;
};

HomeState &home_state();

}  // namespace ui::detail
