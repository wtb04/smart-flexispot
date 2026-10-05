#pragma once

#include "lvgl.h"

#include <cstdint>

// A volume bar for a player whose volume only steps, as a display's does
// through MonitorControl: two buttons, − over its left half and + over its
// right, in place of the level, as there is none to show.
namespace ui::detail {
struct VolumeSteps {
    lv_obj_t *speaker = nullptr;
    lv_obj_t *level   = nullptr;
    lv_obj_t *fill    = nullptr;
    lv_obj_t *down    = nullptr;
    lv_obj_t *up      = nullptr;
    lv_obj_t *divider = nullptr;
};

/** Adds the two buttons to a bar built with its speaker, level and fill. */
VolumeSteps make_volume_steps(lv_obj_t *bar, lv_obj_t *speaker, lv_obj_t *level, lv_obj_t *fill,
                              const lv_font_t *font);

/** The bar as a level, or as the two buttons. */
void show_volume_steps(const VolumeSteps &steps, bool stepping);

/** True while the volume steps: the buttons take the touch, not the bar. */
bool step_volume(lv_obj_t *bar, lv_event_t *e);
}  // namespace ui::detail
