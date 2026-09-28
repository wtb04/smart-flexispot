#include "radar_trail.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace radar {
namespace {
constexpr float KM_PER_DEG = 111.2f;
constexpr float DEG        = 3.14159265f / 180.0f;

float km_between(const TrailPoint &a, float lat, float lon)
{
    const float north = (lat - a.lat) * KM_PER_DEG;
    const float east  = (lon - a.lon) * KM_PER_DEG * std::cos(lat * DEG);
    return std::sqrt(north * north + east * east);
}

// Past the JSON value at p: a string, an array or object with all inside it,
// or a bare number up to the comma or bracket that ends it.
const char *past_value(const char *p, const char *end)
{
    int depth = 0;
    for (; p < end; ++p) {
        if (*p == '"') {
            for (++p; p < end && *p != '"'; ++p) {
                if (*p == '\\') {
                    ++p;
                }
            }
            if (depth == 0) {
                return p < end ? p + 1 : end;
            }
        } else if (*p == '[' || *p == '{') {
            ++depth;
        } else if (*p == ']' || *p == '}') {
            if (depth == 0) {
                return p;
            }
            if (--depth == 0) {
                return p + 1;
            }
        } else if (*p == ',' && depth == 0) {
            return p;
        }
    }
    return end;
}

const char *past_blanks_and_commas(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) {
        ++p;
    }
    return p;
}

// One of the trace's entries, [seconds, lat, lon, ...]: the position, when it has one.
bool read_entry(const char *p, const char *end, float &lat, float &lon)
{
    char *after = nullptr;
    std::strtof(p + 1, &after);
    for (float *field : {&lat, &lon}) {
        if (after == nullptr || after >= end || *after != ',') {
            return false;
        }
        const char *from = after + 1;
        *field           = std::strtof(from, &after);
        if (after == from) {
            return false;  // null
        }
    }
    return true;
}
}  // namespace

void note(Trail &trail, float lat, float lon, float step_km)
{
    if (trail.count > 0) {
        const int last = (trail.head + kTrailPoints - 1) % kTrailPoints;
        if (km_between(trail.points[last], lat, lon) < step_km) {
            return;
        }
    }
    trail.points[trail.head] = {lat, lon};
    trail.head               = (trail.head + 1) % kTrailPoints;
    if (trail.count < kTrailPoints) {
        ++trail.count;
    }
}

int oldest_first(const Trail &trail, TrailPoint *out, int max)
{
    const int count = trail.count < max ? trail.count : max;
    const int first = (trail.head + kTrailPoints - trail.count) % kTrailPoints;
    const int skip  = trail.count - count;  // when out is short, the newest are the ones kept
    for (int i = 0; i < count; ++i) {
        out[i] = trail.points[(first + skip + i) % kTrailPoints];
    }
    return count;
}

int parse_trace(const char *json, std::size_t length, Trail &trail, float step_km)
{
    const char *end = json + length;
    const char *key = json != nullptr ? std::strstr(json, "\"trace\"") : nullptr;
    const char *p   = key != nullptr ? std::strchr(key, '[') : nullptr;
    if (p == nullptr || p >= end) {
        return -1;
    }
    char hex[sizeof(trail.hex)];
    std::memcpy(hex, trail.hex, sizeof(hex));
    const std::int64_t seen = trail.seen_us;
    trail                   = Trail{};
    std::memcpy(trail.hex, hex, sizeof(hex));
    trail.seen_us = seen;

    int positions = 0;
    for (p = past_blanks_and_commas(p + 1, end); p < end && *p == '['; p = past_blanks_and_commas(p, end)) {
        const char *entry_end = past_value(p, end);
        float       lat = 0.0f, lon = 0.0f;
        if (read_entry(p, entry_end, lat, lon)) {
            note(trail, lat, lon, step_km);
            ++positions;
        }
        p = entry_end;
    }
    return positions;
}

}  // namespace radar
