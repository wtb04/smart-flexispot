#pragma once

#include "lvgl.h"
#include "ui.h"

#include <cstdint>

namespace ui {
void build_focus_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** Runs with the LVGL lock held. */
void show_focus(const Focus &focus);

}  // namespace ui
