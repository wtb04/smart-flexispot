#pragma once

#include "ui.h"

// The desk and the lights, as last told: what the rail, the home page and the
// quick actions over the fullscreen views show. What applies the updates
// writes them and publishes Topic::Desk or Topic::Lights. On the LVGL task only.
namespace ui::detail {

struct DeskState {
    int  height_mm = -1;   // in tenths of a centimetre, as the display takes it; -1 unknown
    bool available = true; // the desk answers, so its controls are worth tapping
    bool preset_active[kPresetCount] = {};  // the one it stands at, if any
};

struct LightState {
    char name[48]  = "";  // empty leaves it out
    char state[32] = "";
};

struct LightsState {
    bool       on = false;                 // any of them, as the lights button shows
    bool       light_on[kLightCount] = {}; // each, by its place on the page
    char       label[40] = "";             // the lights button's, and what it says of them
    char       state[32] = "";
    LightState lights[kLightCount];
};

DeskState   &desk_state();
LightsState &lights_state();

}  // namespace ui::detail
