#pragma once

#include "esp_err.h"
#include "radar_parse.h"

#include <cstdint>

namespace radar {

// The feed returns whatever is in range and this decides how many of them get
// kept, drawn and tapped. Each one costs a line and a label on the scope, drawn
// from internal memory: sixty took the low-water mark from 44 KB to 15 KB, and
// this corner of the sky has never held forty at a hundred and sixty
// kilometres.
// How many the feed's answer is boiled down to. Storing one is free -- the
// records live in PSRAM -- so this is set by what is worth knowing about rather
// than by what can be afforded. Drawing is the expensive part and is capped
// separately, on the page.
inline constexpr int kMaxAircraft = 160;

struct Snapshot {
    Aircraft list[kMaxAircraft];
    int      count;
    float    home_lat;
    float    home_lon;
    int      range_km;
    bool     ok;       // whether the last fetch succeeded
    int      age_s;    // since the last good reading, or -1 if there is none
};

/** Called on the radar task after every good reading. */
using UpdateHandler = void (*)(const Snapshot &snapshot);

/** Called on the radar task once a lookup comes back. */
using DetailsHandler = void (*)(const char *hex, const Details &details);

/** RGB565, `width` by `height`. The buffer lives until the next photo
 *  replaces it. Null when the aircraft has no picture on file. */
using PhotoHandler = void (*)(const char *hex, const void *pixels, int width, int height);

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

/** Thread-safe copy of the last good reading. */
void snapshot(Snapshot &out);

}  // namespace radar
