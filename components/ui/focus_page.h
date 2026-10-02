#pragma once

#include "lvgl.h"
#include "ui.h"

#include <cstdint>

namespace ui {
/** The timer fullscreen, over everything but notices, and the card that
 *  drops from the top row's badge. */
void build_focus_full(lv_obj_t *screen);

/** Opens the timer fullscreen, as the badge over other views does. */
void open_focus_full();

/** The card beside the dock, level with `button`, or away again. */
void toggle_focus_popout(lv_obj_t *button, lv_obj_t *dock, void (*lit)(bool open));

/** Whether the timer is open over the whole screen. */
bool focus_full_open();

}  // namespace ui
