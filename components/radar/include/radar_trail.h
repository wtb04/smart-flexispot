#pragma once

#include <cstddef>
#include <cstdint>

// Where an aircraft has been, as the feed has shown it: a point kept each time
// it has gone far enough from the last, the oldest dropped once there are
// kTrailPoints. Pure, so the host tests can run it.
namespace radar {
inline constexpr int kTrailPoints = 128;

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

/** Marks it seen at `now`. After longer than `gap` unseen, the screen dark or
 *  the aircraft out of range, it starts again: joined up, where it was before
 *  and where it is now made a line across ground it never flew over. Any unit
 *  of time, the same for all three. */
void seen(Trail &trail, std::int64_t now, std::int64_t gap);

/** Keeps where it is now, if that is at least `step_km` from the last point. */
void note(Trail &trail, float lat, float lon, float step_km);

/** The points, oldest first; how many were copied. */
int oldest_first(const Trail &trail, TrailPoint *out, int max);

/** Starts `trail` again from a readsb trace, trace_recent_{hex}.json, which
 *  is where the aircraft has been this last quarter hour or so, kept as note()
 *  would. `json` must be NUL-terminated. Returns how many positions the trace
 *  had, or -1 when it is not one. */
int parse_trace(const char *json, std::size_t length, Trail &trail, float step_km);

}  // namespace radar
