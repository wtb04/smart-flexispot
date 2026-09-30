#pragma once

#include "lvgl.h"
#include "radar.h"

#include <cstdint>

namespace ui {
void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** Arriving at the page starts it following the nearest aircraft again. */
void radar_page_opened();

/** Runs with the LVGL lock held. */
void show_radar(const radar::Snapshot &snapshot);

/** Takes radar's own copy of its last reading and shows it. LVGL lock held. */
void refresh_radar();

void show_radar_details(const char *hex, const radar::Details &details);

void show_radar_photo(const char *hex, const void *pixels, int width, int height);

/** Chooses the aircraft as a tap on it would; false when it is not on show.
 *  For the simulator's screenshots. LVGL lock held. */
bool radar_choose(const char *hex);

/** Whether the radar is open over the whole screen. */
bool radar_full_open();

}  // namespace ui
