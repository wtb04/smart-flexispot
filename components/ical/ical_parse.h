#pragma once

#include <cstddef>
#include <cstdint>

namespace ical {
inline constexpr int kSummaryMax  = 96;
inline constexpr int kLocationMax = 32;

struct Event {
    std::int64_t start;  // unix seconds, UTC
    std::int64_t end;
    char         summary[kSummaryMax];
    char         location[kLocationMax];
    std::uint8_t feed;
};

/** Unfolds `body` in place and reads its VEVENTs into `out`, at most `capacity`
 *  of them. Returns how many were stored. An event with no start time is
 *  skipped; one with no end is given its start. */
int parse(char *body, std::size_t length, std::uint8_t feed, Event *out, int capacity);

}  // namespace ical
