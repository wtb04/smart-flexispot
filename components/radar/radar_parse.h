#pragma once

#include <cstddef>

namespace radar {

inline constexpr int kHexLen    = 8;
inline constexpr int kFlightLen = 10;
inline constexpr int kTypeLen   = 8;
inline constexpr int kRegLen    = 10;
inline constexpr int kDescLen   = 28;

struct Aircraft {
    char  hex[kHexLen];
    char  flight[kFlightLen];  // callsign, trailing padding removed
    char  type[kTypeLen];      // ICAO type, "A320" and the like
    char  reg[kRegLen];        // tail number
    char  desc[kDescLen];      // "BOEING 737-800" and the like
    char  category[4];         // ADS-B emitter class, "A3" and the like
    int   squawk;              // -1 when not reported
    float lat;
    float lon;
    float distance_nm;
    float bearing_deg;  // from the point the feed was queried about
    float track_deg;
    float speed_kt;
    int   altitude_ft;
    int   vertical_fpm;
    bool  on_ground;
};

// What adsbdb knows about an aircraft and the flight it is on. Both halves are
// looked up separately and either may be missing: an aircraft the database has
// never seen still flies, and a positioning flight has no route.
struct Details {
    char airline[36];
    char origin_code[8];  // IATA
    char origin_city[28];
    char dest_code[8];
    char dest_city[28];
    char owner[36];
    char manufacturer[20];
    char model[28];
    char photo_url[160];
    bool has_route;
    bool has_aircraft;
    bool photo_checked;  // the photo database has been asked, whatever it said
};

/** Reads /v0/callsign/{callsign}. Leaves the aircraft half of `out` alone. */
bool parse_route(const char *json, std::size_t length, Details &out);

/** Reads /v0/aircraft/{hex}. Leaves the route half of `out` alone. */
bool parse_aircraft(const char *json, std::size_t length, Details &out);

/** Reads planespotters' /pub/photos/hex/{hex} and takes the larger thumbnail's
 *  address. False when the aircraft has no photograph on file, which is an
 *  ordinary answer rather than a failure. */
bool parse_photo(const char *json, std::size_t length, char *out, std::size_t size);

/** Reads an adsb.fi v2 response, which is far too large to hand to a DOM
 *  parser on this part: the allocator keeps anything under sixteen kilobytes
 *  in internal RAM, and a JSON tree of forty kilobytes is thousands of small
 *  nodes. Entries without a position are left out. `json` must be
 *  NUL-terminated. Returns how many were stored, never more than capacity. */
int parse(const char *json, std::size_t length, Aircraft *out, int capacity);

}  // namespace radar
