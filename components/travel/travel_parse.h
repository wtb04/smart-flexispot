#pragma once

#include <cstddef>
#include <cstdint>

namespace travel {
inline constexpr int kLegsMax    = 4;
inline constexpr int kOptionsMax = 3;

inline constexpr int kModeMax  = 8;
inline constexpr int kLineMax  = 14;
inline constexpr int kPlaceMax = 28;

struct Leg {
    char         mode[kModeMax];    // "bus", "train", "walk"
    char         line[kLineMax];    // "1", "Sprinter", ""
    char         from[kPlaceMax];
    char         to[kPlaceMax];
    std::int64_t depart;            // unix seconds
    std::int64_t arrive;
    bool         cancelled;
};

struct Option {
    std::int64_t leave;
    std::int64_t arrive;
    Leg          legs[kLegsMax];
    int          leg_count;
    bool         cancelled;
    bool         late;  // gets there after the time asked for, offered beside the best
};

/** Reads the journeys out of the backend's answer. Times are unix seconds, so
 *  the panel never has to parse a date or know a timezone. Returns how many
 *  options were stored. */
int parse(const char *body, std::size_t length, Option *out, int capacity);

}  // namespace travel
