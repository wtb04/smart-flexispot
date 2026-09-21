#pragma once

#include "lvgl.h"
#include "radar.h"

#include <cstdint>

namespace ui {

void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** Runs with the LVGL lock held. */
void show_radar(const radar::Snapshot &snapshot);

void show_radar_details(const char *hex, const radar::Details &details);

void show_radar_photo(const char *hex, const void *pixels, int width, int height);

}  // namespace ui
