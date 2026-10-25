#pragma once

#include "lvgl.h"
#include "radar.h"

#include <cstdint>

namespace ui {
/** The radar page: the map to the page's every edge, the aircraft chosen in a
 *  column beside it. kit_front_radar_view.cpp */
void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** As a page is chosen: the radar brought up to date for its own, following
 *  the nearest aircraft again unless one was chosen. */
void radar_page_opened();

/** Runs with the LVGL lock held. */
void show_radar(const radar::Snapshot &snapshot);

/** Takes radar's own copy of its last reading and shows it. LVGL lock held. */
void refresh_radar();

void show_radar_details(const char *hex, const radar::Details &details);

void show_radar_photo(const char *hex, const void *pixels, int width, int height, const char *credit);

/** The aircraft chosen as a tap on it would, for the page about to open, and
 *  the range widened as far as it takes to have it on the scope. */
void radar_open_on(const char *hex);

/** Chooses the aircraft as a tap on it would; false when it is not on show.
 *  For the simulator's screenshots. LVGL lock held. */
bool radar_choose(const char *hex);

/** Home's Overhead shows that aircraft while it is in the sky, rather than the
 *  closest, so a screenshot of Home and one of the radar have the same. For
 *  the simulator's screenshots. LVGL lock held. */
void overhead_pin(const char *hex);

}  // namespace ui
