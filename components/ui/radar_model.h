#pragma once

#include "radar.h"

#include <cstdint>

// What was looked up for the aircraft last tapped on the radar: its details,
// and its photo, each as it came and for which aircraft. The updates write
// them and publish Topic::Lookup; the radar page shows them if that aircraft is
// still the one chosen. The traffic itself and the calendar are their own
// components' to keep, and only their changing is told, by Topic::Radar and
// Topic::Calendar. On the LVGL task only.
namespace ui::detail {

struct RadarLookup {
    char            details_hex[radar::kHexLen] = "";
    radar::Details  details{};
    std::uint32_t   details_stamp = 0;
    char            photo_hex[radar::kHexLen] = "";
    const void     *photo        = nullptr;  // RGB565, or null when there is none
    int             photo_width  = 0;
    int             photo_height = 0;
    std::uint32_t   photo_stamp  = 0;
};

const RadarLookup &radar_lookup();

void radar_take_details(const char *hex, const radar::Details &details);
void radar_take_photo(const char *hex, const void *pixels, int width, int height);

}  // namespace ui::detail
