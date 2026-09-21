#include "radar_parse.h"

#include <cstdlib>
#include <cstring>

namespace radar {
namespace {

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
                p += 2;
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

    // Brackets inside strings are stepped over, and counting only the matching
    // pair is enough: the other kind cannot close this one.
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
};

bool key_is(const char *start, std::size_t length, const char *name)
{
    return std::strlen(name) == length && std::strncmp(start, name, length) == 0;
}

// Callsigns arrive padded to eight characters.
void copy_trimmed(char *out, std::size_t size, const char *start, std::size_t length)
{
    while (length > 0 && start[length - 1] == ' ') {
        --length;
    }
    if (length >= size) {
        length = size - 1;
    }
    std::memcpy(out, start, length);
    out[length] = '\0';
}

// Returns false only when the scanner has lost its place, which ends the run;
// an entry that simply has no position leaves `usable` clear and is skipped.
bool read_aircraft(Scanner &in, Aircraft &out, bool &usable)
{
    usable = false;
    out = Aircraft{};
    out.altitude_ft  = -1;
    out.track_deg    = -1.0f;
    out.squawk       = -1;
    bool has_lat     = false;
    bool has_lon     = false;

    if (!in.take('{')) {
        return false;
    }
    if (in.take('}')) {
        return true;
    }

    for (;;) {
        const char *key    = nullptr;
        std::size_t key_len = 0;
        if (!in.string(key, key_len) || !in.take(':')) {
            return false;
        }

        double value = 0.0;
        if (key_is(key, key_len, "hex") && in.peek('"')) {
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            copy_trimmed(out.hex, sizeof(out.hex), text, length);
        } else if (key_is(key, key_len, "flight") && in.peek('"')) {
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            copy_trimmed(out.flight, sizeof(out.flight), text, length);
        } else if (key_is(key, key_len, "t") && in.peek('"')) {
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            copy_trimmed(out.type, sizeof(out.type), text, length);
        } else if (key_is(key, key_len, "r") && in.peek('"')) {
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            copy_trimmed(out.reg, sizeof(out.reg), text, length);
        } else if (key_is(key, key_len, "desc") && in.peek('"')) {
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            copy_trimmed(out.desc, sizeof(out.desc), text, length);
        } else if (key_is(key, key_len, "category") && in.peek('"')) {
            // The emitter class the aircraft broadcasts about itself, which is
            // what decides the shape it gets drawn as.
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            copy_trimmed(out.category, sizeof(out.category), text, length);
        } else if (key_is(key, key_len, "squawk") && in.peek('"')) {
            // Four octal digits, sent as a string.
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            char digits[8] = {};
            copy_trimmed(digits, sizeof(digits), text, length);
            out.squawk = static_cast<int>(std::strtol(digits, nullptr, 10));
        } else if (key_is(key, key_len, "alt_baro") && in.peek('"')) {
            // "ground" stands where a number of feet would otherwise be.
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            out.on_ground   = key_is(text, length, "ground");
            out.altitude_ft = 0;
        } else if (key_is(key, key_len, "alt_baro") && in.number(value)) {
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
        } else if (!in.skip_value()) {
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

struct Field {
    const char *name;
    char       *dest;
    std::size_t size;
};

// Reads the object the scanner is already inside, from just past its '{' to
// its '}', copying whichever listed fields turn up and stepping over the rest.
bool read_fields(Scanner &in, const Field *fields, int count)
{
    if (in.take('}')) {
        return true;
    }
    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.string(key, key_len) || !in.take(':')) {
            return false;
        }

        bool taken = false;
        for (int i = 0; i < count && !taken; ++i) {
            if (!key_is(key, key_len, fields[i].name) || !in.peek('"')) {
                continue;
            }
            const char *text   = nullptr;
            std::size_t length = 0;
            if (!in.string(text, length)) {
                return false;
            }
            copy_trimmed(fields[i].dest, fields[i].size, text, length);
            taken = true;
        }
        if (!taken && !in.skip_value()) {
            return false;
        }

        if (in.take(',')) {
            continue;
        }
        return in.take('}');
    }
}

/** Steps into the object stored under `name`, from just past the parent's '{'. */
bool enter_object(Scanner &in, const char *name)
{
    if (in.peek('}')) {
        return false;
    }
    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.string(key, key_len) || !in.take(':')) {
            return false;
        }
        if (key_is(key, key_len, name)) {
            return in.take('{');
        }
        if (!in.skip_value() || !in.take(',')) {
            return false;
        }
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
        if (!in.string(key, key_len) || !in.take(':')) {
            return false;
        }

        bool handled = false;
        if (in.peek('{')) {
            if (key_is(key, key_len, "airline")) {
                const Field fields[] = {{"name", out.airline, sizeof(out.airline)}};
                handled              = in.take('{') && read_fields(in, fields, 1);
            } else if (key_is(key, key_len, "origin")) {
                const Field fields[] = {
                    {"iata_code", out.origin_code, sizeof(out.origin_code)},
                    {"municipality", out.origin_city, sizeof(out.origin_city)}};
                handled = in.take('{') && read_fields(in, fields, 2);
            } else if (key_is(key, key_len, "destination")) {
                const Field fields[] = {{"iata_code", out.dest_code, sizeof(out.dest_code)},
                                        {"municipality", out.dest_city, sizeof(out.dest_city)}};
                handled = in.take('{') && read_fields(in, fields, 2);
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
    if (!in.take('{')) {
        return false;
    }
    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.string(key, key_len) || !in.take(':')) {
            return false;
        }
        if (key_is(key, key_len, "photos")) {
            break;
        }
        if (!in.skip_value() || !in.take(',')) {
            return false;
        }
    }

    // An aircraft nobody has photographed answers with an empty array.
    if (!in.take('[') || !in.take('{')) {
        return false;
    }
    if (!enter_object(in, "thumbnail_large")) {
        return false;
    }

    const Field fields[] = {{"src", out, size}};
    if (!read_fields(in, fields, 1)) {
        return false;
    }

    // The address comes back with its slashes escaped, which no url wants.
    char *write = out;
    for (const char *read = out; *read != '\0'; ++read) {
        if (*read == '\\' && read[1] != '\0') {
            ++read;
        }
        *write++ = *read;
    }
    *write = '\0';
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
    if (!read_fields(in, fields, 4)) {
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

    // Walk the top-level object for "aircraft" rather than assuming where it
    // sits; the feed puts a timestamp and a count around it.
    if (!in.take('{')) {
        return 0;
    }
    for (;;) {
        const char *key     = nullptr;
        std::size_t key_len = 0;
        if (!in.string(key, key_len) || !in.take(':')) {
            return 0;
        }
        if (key_is(key, key_len, "aircraft") || key_is(key, key_len, "ac")) {
            break;
        }
        if (!in.skip_value()) {
            return 0;
        }
        if (!in.take(',')) {
            return 0;
        }
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
            // A malformed entry ends the run rather than poisoning the rest:
            // the scanner no longer knows where it is.
            break;
        }
        if (usable && stored < capacity) {
            out[stored++] = aircraft;
        }
        if (in.take(',')) {
            continue;
        }
        break;
    }
    return stored;
}

}  // namespace radar
