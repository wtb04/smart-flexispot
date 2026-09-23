#pragma once

#include "esp_err.h"
#include "ical_parse.h"

#include <cstdint>

namespace ical {
/** How many events are kept across the feeds, oldest first. The term's
 *  timetable runs to about sixty and the work calendar to about a hundred. */
inline constexpr int kMaxEvents = 320;

inline constexpr int kFeedCount = 5;

/** The name each feed is shown under, indexed by Event::feed. */
const char *feed_name(std::uint8_t feed);

/** Called on the calendar task whenever a round of fetching finishes. */
using UpdateHandler = void (*)();

/** Requires the network and a set clock. */
esp_err_t start(UpdateHandler on_update);

/** Copies the events that have not finished yet into `out`, soonest first, and
 *  returns how many. Thread-safe. */
int upcoming(Event *out, int capacity);

/** Copies the events that overlap [from, to), finished or not, soonest first,
 *  and returns how many. Thread-safe. */
int between(std::int64_t from, std::int64_t to, Event *out, int capacity);

/** Fetches now rather than on the next round. */
void refresh();

}  // namespace ical
