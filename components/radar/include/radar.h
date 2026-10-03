#pragma once

#include "esp_err.h"
#include "radar_parse.h"
#include "radar_trail.h"

#include <cstdint>

namespace radar {
// Measured: about 165 airborne within 160 km on a weekday afternoon, and the
// feed's busiest hours bring more; the 250 km now asked about is some 2.4
// times the sky, which this holds with room over.
inline constexpr int kMaxAircraft = 640;

struct Snapshot {
    Aircraft list[kMaxAircraft];
    int      count;
    float    home_lat;
    float    home_lon;
    int      range_km;
    bool     ok;       // whether the last fetch succeeded
    int      age_s;    // since the last good reading, or -1 if there is none
};

/** What the scope is doing, without the aircraft. Cheap enough for a small
 *  stack, which a whole Snapshot is not. */
struct Status {
    int  count;
    int  range_km;
    bool ok;
    int  age_s;
};

/** Called on the radar task after every good reading. */
using UpdateHandler = void (*)(const Snapshot &snapshot);

/** Called on the radar task once a lookup comes back. */
using DetailsHandler = void (*)(const char *hex, const Details &details);

/** RGB565, `width` by `height`. The buffer lives until the next photo
 *  replaces it. Null when the aircraft has no picture on file. `credit` is
 *  its photographer, to name with it, or empty. */
using PhotoHandler = void (*)(const char *hex, const void *pixels, int width, int height, const char *credit);

/** Looks up who is flying and where from and to. One at a time: a request
 *  replaces whatever was waiting, since only the last tap matters. */
void request_details(const char *hex, const char *callsign);

/** Needs the network. Nothing is fetched until a home position arrives. */
esp_err_t start(UpdateHandler on_update, DetailsHandler on_details,
                PhotoHandler on_photo);

/** Fast while the scope is on screen, slow enough to stay roughly current
 *  while it is not. Turning it on fetches at once rather than waiting. */
void set_active(bool active);

/** A page nobody can reach needs nothing fetched for it at all. */
void set_enabled(bool enabled);

/** Taken from Home Assistant's zone.home, so the panel is not told twice where
 *  it is. Safe from any task. */
void set_home(float lat, float lon);

/** Where `hex` has been while the feed has seen it, oldest first, up to `max`
 *  points; how many there are. Thread-safe. */
int trail(const char *hex, TrailPoint *out, int max);

/** Thread-safe copy of the last good reading. */
void snapshot(Snapshot &out);

/** Thread-safe. */
void status(Status &out);

}  // namespace radar
