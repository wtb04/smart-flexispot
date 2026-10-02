#pragma once

#include "lvgl.h"

#include <cstdint>

namespace ui {
void build_calendar_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** Home's tile of what comes next, and the few after it; a tap to the calendar. */
void build_next_tile(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h);

/** Runs with the LVGL lock held. Re-reads whatever the calendar has. */
void show_calendar();

}  // namespace ui
