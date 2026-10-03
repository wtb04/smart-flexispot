#pragma once

#include "ui.h"

// The desk and the lights, as last told: what the dock, the control bar and the
// quick actions over the fullscreen views show. Updates come in through the
// take_ functions, and a preset tapped through desk_go_to; each publishes
// Topic::Desk or Topic::Lights. On the LVGL task only.
namespace ui::detail {

struct DeskState {
    int  height_mm = -1;   // in tenths of a centimetre, as the display takes it; -1 unknown
    bool available = true; // the desk answers, so its controls are worth tapping
    bool preset_active[kPresetCount] = {};  // the one it stands at, if any
    int  travelling = -1;  // the preset tapped and not yet reached, or -1
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

const DeskState   &desk_state();
const LightsState &lights_state();

void desk_take_active(int index, bool active);
void desk_take_height(int height_mm);
void desk_take_available(bool available);
/** A preset tapped: the desk goes there, or, while it is on its way there,
 *  stops, as the control box's own buttons do. */
void desk_go_to(int index);

void lights_take(const char *label, const char *state, bool on);
void lights_take_light(int index, const char *name, const char *state, bool on);

}  // namespace ui::detail
