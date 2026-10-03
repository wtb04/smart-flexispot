#pragma once

#include <cstddef>

namespace radar {
inline constexpr int kHexLen      = 8;
inline constexpr int kFlightLen   = 10;
inline constexpr int kTypeLen     = 8;
inline constexpr int kRegLen      = 10;
inline constexpr int kDescLen     = 28;
inline constexpr int kCategoryLen = 4;

inline constexpr int kAirlineLen      = 36;
inline constexpr int kAirportCodeLen  = 8;
inline constexpr int kCityLen         = 28;
inline constexpr int kOwnerLen        = 36;
inline constexpr int kManufacturerLen = 20;
inline constexpr int kModelLen        = 28;
inline constexpr int kPhotoUrlLen     = 160;

struct Aircraft {
    char  hex[kHexLen];
    char  flight[kFlightLen];      // callsign, trailing padding removed
    char  type[kTypeLen];          // ICAO type, "A320" and the like
    char  reg[kRegLen];            // tail number
    char  desc[kDescLen];          // "BOEING 737-800" and the like
    char  category[kCategoryLen];  // ADS-B emitter class, "A3" and the like
    int   squawk;                  // -1 when not reported
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

struct Details {
    char airline[kAirlineLen];
    char origin_code[kAirportCodeLen];  // IATA
    char origin_city[kCityLen];
    char dest_code[kAirportCodeLen];
    char dest_city[kCityLen];
    float origin_lat;  // where the airports are, when the route says
    float origin_lon;
    float dest_lat;
    float dest_lon;
    bool  has_origin_at;
    bool  has_dest_at;
    char owner[kOwnerLen];
    char manufacturer[kManufacturerLen];
    char model[kModelLen];
    char photo_url[kPhotoUrlLen];
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

/** Whether the route fits where the aircraft is: near the great circle from
 *  one airport to the other, and not past either end. A callsign's route can
 *  be another day's leg, as airlines give their callsigns to other flights; one
 *  that does not fit is not to be shown. True when the airports' places are not
 *  known, as nothing then says it is wrong. */
bool route_fits(const Details &details, float lat, float lon);

/** Worth following of its own accord: in the air, and an airline's flight, by
 *  a callsign of three letters and then a number, or large or heavy by its
 *  ADS-B category. Not a light aircraft calling by its registration. */
bool interesting(const Aircraft &aircraft);

/** Reads an adsb.fi v2 response, which is far too large to hand to a DOM
 *  parser on this part: the allocator keeps anything under sixteen kilobytes
 *  in internal RAM, and a JSON tree of forty kilobytes is thousands of small
 *  nodes. Entries without a position are left out. `json` must be
 *  NUL-terminated. Returns how many were stored, never more than capacity. */
int parse(const char *json, std::size_t length, Aircraft *out, int capacity);

}  // namespace radar
