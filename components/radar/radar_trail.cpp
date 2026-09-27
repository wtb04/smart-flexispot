#include "radar_trail.h"

#include <cmath>

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

}  // namespace radar
