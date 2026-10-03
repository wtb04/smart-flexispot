#pragma once

#include "radar.h"

#include <cstdint>
#include <vector>

// What live.cpp's threads call, one fetch at a time.
namespace live {
/** Every calendar feed; false when one could not be had, which keeps its last events. */
bool fetch_calendar();

/** The way to the next appointment, when it is soon enough to ask; false when
 *  there is nothing to ask or no answer. */
bool fetch_journey();

/** The sky round home; false with `busy` when the feed asked to be left alone. */
bool fetch_sky(bool &busy);

/** Who a plane is and where it flies, from the lookup service. */
bool fetch_details(const char *hex, const char *callsign, radar::Details &out);

/** Where it has been this last quarter hour, as its trail; false when unknown. */
bool fetch_trace(const char *hex);

/** Its photo as RGB565, no larger than the panel shows it; empty for none. */
std::vector<std::uint16_t> fetch_photo(const char *hex, int &width, int &height, std::string &credit);
}  // namespace live
