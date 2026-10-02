#pragma once

#include "lvgl.h"
#include "radar.h"

#include <cstdint>

namespace ui {
void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height);

/** Where on Home the radar's cards go, `w` by `h` at x, y of `parent`, before
 *  the radar page is built; on the radar page itself it is the map. */
/** As wide as the cards are with a scope `height` high. */
std::int32_t radar_cards_width(std::int32_t height);
void radar_home_area(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h);

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
