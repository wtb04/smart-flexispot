#pragma once

#include "lvgl.h"

#include <cstdint>

namespace ui {
void build_calendar_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** Runs with the LVGL lock held. Re-reads whatever the calendar has. */
void show_calendar();

}  // namespace ui
