#pragma once

#include <cstdint>

// Where an aircraft has been, as the feed has shown it: a point kept each time
// it has gone far enough from the last, the oldest dropped once there are
// kTrailPoints. Pure, so the host tests can run it.
namespace radar {
inline constexpr int kTrailPoints = 64;

struct TrailPoint {
    float lat;
    float lon;
};

struct Trail {
    char         hex[8];
    std::int64_t seen_us;  // when the feed last had it
    int          count;
    int          head;  // where the next point goes
    TrailPoint   points[kTrailPoints];
};

/** Keeps where it is now, if that is at least `step_km` from the last point. */
void note(Trail &trail, float lat, float lon, float step_km);

/** The points, oldest first; how many were copied. */
int oldest_first(const Trail &trail, TrailPoint *out, int max);

}  // namespace radar
