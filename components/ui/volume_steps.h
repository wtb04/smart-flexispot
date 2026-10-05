#pragma once

#include "lvgl.h"

#include <cstdint>

// A volume bar for a player whose volume only steps, as a display's does
// through MonitorControl: − and + at its ends, the speaker between them, and
// no level, as there is none to show.
namespace ui::detail {
struct VolumeSteps {
    lv_obj_t    *speaker = nullptr;
    lv_obj_t    *level   = nullptr;
    lv_obj_t    *fill    = nullptr;
    lv_obj_t    *minus   = nullptr;
    lv_obj_t    *plus    = nullptr;
    std::int32_t inset   = 0;
};

/** Adds the − and + to a bar built with its speaker, level and fill. */
VolumeSteps make_volume_steps(lv_obj_t *bar, lv_obj_t *speaker, lv_obj_t *level, lv_obj_t *fill,
                              const lv_font_t *font, std::int32_t inset);

/** The bar as a level, or as steps. */
void show_volume_steps(const VolumeSteps &steps, bool stepping);

/** A touch on a bar while the volume steps: a press on its left half steps
 *  down, on its right half up. False when it does not step, for the bar to
 *  take the touch as a level. */
bool step_volume(lv_obj_t *bar, lv_event_t *e);
}  // namespace ui::detail
