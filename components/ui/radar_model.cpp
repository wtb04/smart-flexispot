#include "radar_model.h"

#include "topics.h"

#include <cstdio>

namespace ui::detail {
namespace {
RadarLookup s_lookup;
}  // namespace

const RadarLookup &radar_lookup()
{
    return s_lookup;
}

void radar_take_details(const char *hex, const radar::Details &details)
{
    std::snprintf(s_lookup.details_hex, sizeof(s_lookup.details_hex), "%s", hex != nullptr ? hex : "");
    s_lookup.details = details;
    ++s_lookup.details_stamp;
    publish(Topic::Lookup);
}

void radar_take_photo(const char *hex, const void *pixels, int width, int height)
{
    std::snprintf(s_lookup.photo_hex, sizeof(s_lookup.photo_hex), "%s", hex != nullptr ? hex : "");
    s_lookup.photo        = pixels;
    s_lookup.photo_width  = width;
    s_lookup.photo_height = height;
    ++s_lookup.photo_stamp;
    publish(Topic::Lookup);
}

}  // namespace ui::detail
