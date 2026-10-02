#pragma once

#include "lvgl.h"
#include "ui.h"

#include <cstdint>

namespace ui {
void build_focus_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** The timer fullscreen, over everything but notices; opened from the dial. */
void build_focus_full(lv_obj_t *screen);

/** Opens the timer fullscreen, as the badge over other views does. */
void open_focus_full();

/** Whether the timer is open over the whole screen. */
bool focus_full_open();

}  // namespace ui
