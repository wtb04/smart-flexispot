#include "radar_parse.h"

#include <algorithm>
#include <cmath>

#include <cstdlib>
#include <cstring>

namespace radar {
namespace {
// A backslash and the character it escapes.
constexpr int ESCAPE_LENGTH = 2;

constexpr std::size_t SQUAWK_TEXT_SIZE = 8;

struct Scanner {
    const char *p;
    const char *end;

    void skip_space()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }

    bool take(char c)
    {
        skip_space();
        if (p < end && *p == c) {
            ++p;
            return true;
        }
        return false;
    }

    bool peek(char c)
    {
        skip_space();
        return p < end && *p == c;
    }

    bool string(const char *&start, std::size_t &length)
    {
        skip_space();
        if (p >= end || *p != '"') {
            return false;
        }
        ++p;
        start = p;
        while (p < end) {
            if (*p == '\\') {
                p += ESCAPE_LENGTH;
                continue;
            }
            if (*p == '"') {
                length = static_cast<std::size_t>(p - start);
                ++p;
                return true;
            }
            ++p;
        }
        return false;
    }

    bool number(double &out)
    {
        skip_space();
        char *stop = nullptr;
        out        = std::strtod(p, &stop);
        if (stop == nullptr || stop == p) {
            return false;
        }
        p = stop;
        return true;
    }

    bool skip_value()
    {
        skip_space();
        if (p >= end) {
            return false;
        }
        if (*p == '"') {
            const char *start  = nullptr;
            std::size_t length = 0;
            return string(start, length);
        }
        if (*p == '{' || *p == '[') {
            const char open  = *p;
            const char close = open == '{' ? '}' : ']';
            int        depth = 0;
            while (p < end) {
                if (*p == '"') {
                    const char *start  = nullptr;
                    std::size_t length = 0;
                    if (!string(start, length)) {
                        return false;
                    }
                    continue;
                }
                if (*p == open) {
                    ++depth;
                } else if (*p == close) {
                    --depth;
                    if (depth == 0) {
                        ++p;
                        return true;
                    }
                }
                ++p;
            }
            return false;
        }
        while (p < end && *p != ',' && *p != '}' && *p != ']') {
            ++p;
        }
        return true;
    }

    /** Reads `"key":`, leaving the scanner at the value. */
    bool key(const char *&start, std::size_t &length)
    {
        return string(start, length) && take(':');
    }
};

bool key_is(const char *start, std::size_t length, const char *name)
{
    return std::strlen(name) == length && std::strncmp(start, name, length) == 0;
}

// A callsign comes padded with spaces, or with '@', the blank of ADS-B's own
// character set, which some transponders send in place of any callsign at all.
void copy_trimmed(char *out, std::size_t size, const char *start, std::size_t length)
{
    while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '@')) {
        --length;
    }
    if (length >= size) {
        length = size - 1;
    }
    std::memcpy(out, start, length);
    out[length] = '\0';
}

bool read_text(Scanner &in, char *out, std::size_t size)
{
    const char *text   = nullptr;
    std::size_t length = 0;
    if (!in.string(text, length)) {
        return false;
    }
    copy_trimmed(out, size, text, length);
    return true;
}

struct Field {
    const char *name;
    char       *dest;
    std::size_t size;
};

/** The field named by the key, when its value is a string. */
template <std::size_t N>
const Field *text_field(Scanner &in, const Field (&fields)[N], const char *key,
                        std::size_t key_len)
{
    for (const Field &field : fields) {
        if (key_is(key, key_len, field.name) && in.peek('"')) {
            return &field;
        }
    }
    return nullptr;
}

bool read_number_field(Scanner &in, const char *key, std::size_t key_len, Aircraft &out,
                       bool &has_lat, bool &has_lon)
{
    double value = 0.0;
    if (key_is(key, key_len, "alt_baro") && in.number(value)) {
        out.altitude_ft = static_cast<int>(value);
    } else if (key_is(key, key_len, "lat") && in.number(value)) {
        out.lat = static_cast<float>(value);
        has_lat = true;
    } else if (key_is(key, key_len, "lon") && in.number(value)) {
        out.lon = static_cast<float>(value);
        has_lon = true;
    } else if (key_is(key, key_len, "dst") && in.number(value)) {
        out.distance_nm = static_cast<float>(value);
    } else if (key_is(key, key_len, "dir") && in.number(value)) {
        out.bearing_deg = static_cast<float>(value);
    } else if (key_is(key, key_len, "track") && in.number(value)) {
        out.track_deg = static_cast<float>(value);
    } else if (key_is(key, key_len, "gs") && in.number(value)) {
        out.speed_kt = static_cast<float>(value);
    } else if (key_is(key, key_len, "baro_rate") && in.number(value)) {
        out.vertical_fpm = static_cast<int>(value);
    } else {
        return false;
    }
    return true;
}

bool read_aircraft(Scanner &in, Aircraft &out, bool &usable)
{
    usable          = false;
    out             = Aircraft{};
    out.altitude_ft = -1;
    out.track_deg   = -1.0f;
    out.squawk      = -1;
    bool has_lat    = false;
    bool has_lon    = false;

    const Field text_fields[] = {
        {"hex", out.hex, sizeof(out.hex)},
        {"flight", out.flight, sizeof(out.flight)},
        {"t", out.type, sizeof(out.type)},
        {"r", out.reg, sizeof(out.reg)},
        {"desc", out.desc, sizeof(out.desc)},
        {"category", out.category, sizeof(out.category)},
    };

    if (!in.take('{')) {
        return false;
    }
    if (in.take('}')) {
        return true;
    }

    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.key(key, key_len)) {
            return false;
        }

        if (const Field *field = text_field(in, text_fields, key, key_len)) {
            if (!read_text(in, field->dest, field->size)) {
                return false;
            }
        } else if (key_is(key, key_len, "squawk") && in.peek('"')) {
            char digits[SQUAWK_TEXT_SIZE] = {};
            if (!read_text(in, digits, sizeof(digits))) {
                return false;
            }
            out.squawk = static_cast<int>(std::strtol(digits, nullptr, 10));
        } else if (key_is(key, key_len, "alt_baro") && in.peek('"')) {
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            out.on_ground   = key_is(text, length, "ground");
            out.altitude_ft = 0;
        } else if (!read_number_field(in, key, key_len, out, has_lat, has_lon) &&
                   !in.skip_value()) {
            return false;
        }

        if (in.take(',')) {
            continue;
        }
        if (in.take('}')) {
            break;
        }
        return false;
    }

    usable = has_lat && has_lon;
    return true;
}

/** An airport of the route: its code and town, and where it is. */
struct Place {
    char       *code;
    std::size_t code_size;
    char       *city;
    std::size_t city_size;
    float      &lat;
    float      &lon;
    bool       &has_at;
};

bool read_place(Scanner &in, const Place &place)
{
    if (in.take('}')) {
        return true;
    }
    bool has_lat = false;
    bool has_lon = false;
    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.key(key, key_len)) {
            return false;
        }
        double value = 0.0;
        bool   read  = true;
        if (key_is(key, key_len, "iata_code") && in.peek('"')) {
            read = read_text(in, place.code, place.code_size);
        } else if (key_is(key, key_len, "municipality") && in.peek('"')) {
            read = read_text(in, place.city, place.city_size);
        } else if (key_is(key, key_len, "latitude") && in.number(value)) {
            place.lat = static_cast<float>(value);
            has_lat   = true;
        } else if (key_is(key, key_len, "longitude") && in.number(value)) {
            place.lon = static_cast<float>(value);
            has_lon   = true;
        } else {
            read = in.skip_value();
        }
        if (!read) {
            return false;
        }
        if (in.take(',')) {
            continue;
        }
        place.has_at = has_lat && has_lon;
        return in.take('}');
    }
}

template <std::size_t N>
bool read_fields(Scanner &in, const Field (&fields)[N])
{
    if (in.take('}')) {
        return true;
    }
    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.key(key, key_len)) {
            return false;
        }

        if (const Field *field = text_field(in, fields, key, key_len)) {
            if (!read_text(in, field->dest, field->size)) {
                return false;
            }
        } else if (!in.skip_value()) {
            return false;
        }

        if (in.take(',')) {
            continue;
        }
        return in.take('}');
    }
}

/** Moves past the key `name` (or `alias`) of the object being read, leaving
 *  the scanner at its value. */
bool seek_key(Scanner &in, const char *name, const char *alias = nullptr)
{
    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.key(key, key_len)) {
            return false;
        }
        if (key_is(key, key_len, name) || (alias != nullptr && key_is(key, key_len, alias))) {
            return true;
        }
        if (!in.skip_value() || !in.take(',')) {
            return false;
        }
    }
}

/** Steps into the object stored under `name`, from just past the parent's '{'. */
bool enter_object(Scanner &in, const char *name)
{
    if (in.peek('}')) {
        return false;
    }
    return seek_key(in, name) && in.take('{');
}

void unescape_in_place(char *text)
{
    char *write = text;
    for (const char *read = text; *read != '\0'; ++read) {
        if (*read == '\\' && read[1] != '\0') {
            ++read;
        }
        *write++ = *read;
    }
    *write = '\0';
}

/** Stores the aircraft, or, once full, lets it replace the farthest if it is
 *  nearer: the scope shows the nearest `capacity`. */
void keep_nearest(Aircraft *out, int capacity, int &stored, const Aircraft &aircraft)
{
    if (stored < capacity) {
        out[stored++] = aircraft;
        return;
    }
    int farthest = 0;
    for (int i = 1; i < capacity; ++i) {
        if (out[i].distance_nm > out[farthest].distance_nm) {
            farthest = i;
        }
    }
    if (aircraft.distance_nm < out[farthest].distance_nm) {
        out[farthest] = aircraft;
    }
}

}  // namespace

bool parse_route(const char *json, std::size_t length, Details &out)
{
    if (json == nullptr) {
        return false;
    }
    Scanner in{json, json + length};
    if (!in.take('{') || !enter_object(in, "response") || !enter_object(in, "flightroute")) {
        return false;
    }

    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.key(key, key_len)) {
            return false;
        }

        bool handled = false;
        if (in.peek('{')) {
            if (key_is(key, key_len, "airline")) {
                const Field fields[] = {{"name", out.airline, sizeof(out.airline)}};
                handled              = in.take('{') && read_fields(in, fields);
            } else if (key_is(key, key_len, "origin")) {
                const Place place{out.origin_code, sizeof(out.origin_code), out.origin_city,
                                  sizeof(out.origin_city), out.origin_lat, out.origin_lon,
                                  out.has_origin_at};
                handled = in.take('{') && read_place(in, place);
            } else if (key_is(key, key_len, "destination")) {
                const Place place{out.dest_code, sizeof(out.dest_code), out.dest_city,
                                  sizeof(out.dest_city), out.dest_lat, out.dest_lon, out.has_dest_at};
                handled = in.take('{') && read_place(in, place);
            }
        }
        if (!handled && !in.skip_value()) {
            return false;
        }

        if (in.take(',')) {
            continue;
        }
        break;
    }

    out.has_route = out.origin_code[0] != '\0' || out.dest_code[0] != '\0';
    return out.has_route;
}

bool parse_photo(const char *json, std::size_t length, char *out, std::size_t size)
{
    if (json == nullptr || out == nullptr || size == 0) {
        return false;
    }
    out[0] = '\0';

    Scanner in{json, json + length};
    if (!in.take('{') || !seek_key(in, "photos")) {
        return false;
    }
    if (!in.take('[') || !in.take('{')) {
        return false;
    }
    if (!enter_object(in, "thumbnail_large")) {
        return false;
    }

    const Field fields[] = {{"src", out, size}};
    if (!read_fields(in, fields)) {
        return false;
    }
    unescape_in_place(out);
    return out[0] != '\0';
}

bool parse_aircraft(const char *json, std::size_t length, Details &out)
{
    if (json == nullptr) {
        return false;
    }
    Scanner in{json, json + length};
    if (!in.take('{') || !enter_object(in, "response") || !enter_object(in, "aircraft")) {
        return false;
    }

    const Field fields[] = {
        {"manufacturer", out.manufacturer, sizeof(out.manufacturer)},
        {"type", out.model, sizeof(out.model)},
        {"registered_owner", out.owner, sizeof(out.owner)},
        {"url_photo_thumbnail", out.photo_url, sizeof(out.photo_url)},
    };
    if (!read_fields(in, fields)) {
        return false;
    }
    out.has_aircraft = out.model[0] != '\0' || out.owner[0] != '\0';
    return out.has_aircraft;
}

int parse(const char *json, std::size_t length, Aircraft *out, int capacity)
{
    if (json == nullptr || out == nullptr || capacity <= 0) {
        return 0;
    }

    Scanner in{json, json + length};
    if (!in.take('{') || !seek_key(in, "aircraft", "ac")) {
        return 0;
    }
    if (!in.take('[')) {
        return 0;
    }
    if (in.take(']')) {
        return 0;
    }

    int stored = 0;
    for (;;) {
        Aircraft aircraft{};
        bool     usable = false;
        if (!read_aircraft(in, aircraft, usable)) {
            break;
        }
        if (usable && !aircraft.on_ground) {
            keep_nearest(out, capacity, stored, aircraft);
        }
        if (in.take(',')) {
            continue;
        }
        break;
    }
    return stored;
}

namespace {
struct Unit {
    double x, y, z;
};

Unit unit_at(double lat_deg, double lon_deg)
{
    constexpr double DEG_RAD = 3.14159265358979323846 / 180.0;
    const double     lat     = lat_deg * DEG_RAD;
    const double     lon     = lon_deg * DEG_RAD;
    return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

Unit cross(const Unit &a, const Unit &b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double dot(const Unit &a, const Unit &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
}  // namespace

bool route_fits(const Details &details, float lat, float lon)
{
    constexpr double EARTH_KM     = 6371.0;
    constexpr double SLACK_KM     = 150.0;  // airways are not great circles, and a flight starts and ends off the line
    constexpr double SLACK_SHARE  = 0.15;   // of a long route's length
    if (!details.has_route || !details.has_origin_at || !details.has_dest_at) {
        return true;
    }
    const Unit   from   = unit_at(details.origin_lat, details.origin_lon);
    const Unit   to     = unit_at(details.dest_lat, details.dest_lon);
    const Unit   here   = unit_at(lat, lon);
    Unit         normal = cross(from, to);
    const double length = std::sqrt(dot(normal, normal));
    if (length < 1e-9) {
        return true;  // the same airport twice, or the far side of the world
    }
    normal = {normal.x / length, normal.y / length, normal.z / length};
    const double route_km = std::atan2(length, dot(from, to)) * EARTH_KM;
    const double slack_km = std::max(SLACK_KM, SLACK_SHARE * route_km);
    const double off      = dot(here, normal);
    const double off_km   = std::asin(std::clamp(off, -1.0, 1.0)) * EARTH_KM;
    // Along the route from its origin, of where the aircraft is put on the line.
    const Unit on{here.x - off * normal.x, here.y - off * normal.y, here.z - off * normal.z};
    const double along_km = std::atan2(dot(cross(from, on), normal), dot(from, on)) * EARTH_KM;
    return std::fabs(off_km) <= slack_km && along_km >= -slack_km && along_km <= route_km + slack_km;
}

}  // namespace radar
