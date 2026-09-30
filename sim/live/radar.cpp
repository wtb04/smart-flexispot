// The sky round home, fetched from the feeds radar.cpp polls and read by the
// same parser; the trails kept by the same code; a tapped plane looked up where
// the panel looks it up. Home is SIM_HOME, "lat,lon", where the panel has
// Home Assistant's zone.home; without it nothing is fetched.
#include "feeds.h"
#include "http.h"
#include "radar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <ctime>

#if __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace radar {
namespace {
// As radar.cpp: two feeds of the same data, taken in turn, the other asked at
// once when one will not answer.
constexpr const char  *FEEDS[]        = {"https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
                                         "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d"};
constexpr char         LOOKUP_HOST[]  = "https://api.adsbdb.com";
constexpr char         PHOTO_HOST[]   = "https://api.planespotters.net";
constexpr char         TRACE_HOST[]   = "https://adsb.lol";
constexpr int          RANGE_KM       = 250;  // as components/radar asks
constexpr float        NM_PER_KM      = 0.539957f;
constexpr float        TRAIL_STEP_KM  = 2.0f;
constexpr int          PHOTO_MAX_W    = 320;
constexpr int          PHOTO_MAX_H    = 240;
constexpr std::int64_t TRAIL_FORGET_S = 15 * 60; // unheard of this long, its trail goes
constexpr std::int64_t TRAIL_GAP_S    = 3 * 60;  // unseen this long, it starts again

std::mutex                             s_lock;
Snapshot                               s_snapshot{};
std::int64_t                           s_fetched_at = 0; // unix seconds of the last good reading
std::unordered_map<std::string, Trail> s_trails;

bool home(float &lat, float &lon)
{
    const char *text = std::getenv("SIM_HOME");
    return text != nullptr && std::sscanf(text, "%f,%f", &lat, &lon) == 2;
}
} // namespace

void snapshot(Snapshot &out)
{
    std::lock_guard<std::mutex> hold(s_lock);
    out       = s_snapshot;
    out.age_s = s_fetched_at > 0 ? static_cast<int>(std::time(nullptr) - s_fetched_at) : -1;
}

int trail(const char *hex, TrailPoint *out, int max)
{
    std::lock_guard<std::mutex> hold(s_lock);
    const auto                  found = s_trails.find(hex);
    return found != s_trails.end() ? oldest_first(found->second, out, max) : 0;
}
} // namespace radar

namespace live {
bool fetch_sky(bool &busy)
{
    busy      = false;
    float lat = 0.0f, lon = 0.0f;
    if (!radar::home(lat, lon)) {
        return false;
    }
    static int next    = 0;
    Answer     got;
    int        refused = 0;
    for (int tried = 0; tried < 2 && got.status != 200; ++tried) {
        char url[128];
        std::snprintf(url, sizeof(url), radar::FEEDS[next], lat, lon,
                      static_cast<int>(std::lround(radar::RANGE_KM * radar::NM_PER_KM)));
        next = 1 - next;
        got  = get(url);
        refused += got.status == 429 ? 1 : 0;
    }
    busy = refused == 2;

    static radar::Aircraft list[radar::kMaxAircraft];
    const int count = got.status == 200 ? radar::parse(got.body.data(), got.body.size(), list,
                                                       radar::kMaxAircraft)
                                        : 0;

    std::lock_guard<std::mutex> hold(radar::s_lock);
    radar::Snapshot            &s = radar::s_snapshot;
    s.home_lat                    = lat;
    s.home_lon                    = lon;
    s.range_km                    = radar::RANGE_KM;
    s.ok                          = got.status == 200;
    if (!s.ok) {
        std::printf("W (radar) feed: %d\n", got.status);
        return false;
    }
    std::memcpy(s.list, list, sizeof(radar::Aircraft) * static_cast<std::size_t>(count));
    s.count             = count;
    radar::s_fetched_at = std::time(nullptr);
    for (int i = 0; i < count; ++i) {
        if (!list[i].on_ground && list[i].hex[0] != '\0') {
            radar::Trail &trail = radar::s_trails[list[i].hex];
            std::snprintf(trail.hex, sizeof(trail.hex), "%s", list[i].hex);
            radar::seen(trail, radar::s_fetched_at, radar::TRAIL_GAP_S); // seconds here
            radar::note(trail, list[i].lat, list[i].lon, radar::TRAIL_STEP_KM);
        }
    }
    std::erase_if(radar::s_trails, [](const auto &entry) {
        return radar::s_fetched_at - entry.second.seen_us > radar::TRAIL_FORGET_S;
    });
    return true;
}

// As radar.cpp's fetch_details: one request for both halves, and each again on
// its own when the other was unknown.
bool fetch_details(const char *hex, const char *callsign, radar::Details &out)
{
    bool want_aircraft = true;
    bool want_route    = callsign[0] != '\0';
    char url[192];
    if (want_route) {
        std::snprintf(url, sizeof(url), "%s/v0/aircraft/%s?callsign=%s", radar::LOOKUP_HOST, hex,
                      callsign);
        const Answer got = get(url, {}, net::Priority::Tap);
        if (got.status == 200) {
            radar::parse_aircraft(got.body.data(), got.body.size(), out);
            radar::parse_route(got.body.data(), got.body.size(), out);
            return true;
        }
        if (got.status != 404) {
            return false;
        }
        want_route    = got.body.find("unknown callsign") == std::string::npos;
        want_aircraft = got.body.find("unknown aircraft") == std::string::npos;
    }
    if (want_aircraft) {
        std::snprintf(url, sizeof(url), "%s/v0/aircraft/%s", radar::LOOKUP_HOST, hex);
        if (const Answer got = get(url, {}, net::Priority::Tap); got.status == 200) {
            radar::parse_aircraft(got.body.data(), got.body.size(), out);
        }
    }
    if (want_route) {
        std::snprintf(url, sizeof(url), "%s/v0/callsign/%s", radar::LOOKUP_HOST, callsign);
        if (const Answer got = get(url, {}, net::Priority::Tap); got.status == 200) {
            radar::parse_route(got.body.data(), got.body.size(), out);
        }
    }
    return out.has_aircraft || out.has_route;
}

// As radar.cpp's fetch_trace: where the plane has been this last quarter hour.
bool fetch_trace(const char *hex)
{
    const std::size_t len = std::strlen(hex);
    if (len < 2 || hex[0] == '~') {
        return false;
    }
    char url[128];
    std::snprintf(url, sizeof(url), "%s/data/traces/%s/trace_recent_%s.json", radar::TRACE_HOST, hex + len - 2,
                  hex);
    const Answer got = get(url, {}, net::Priority::Tap);
    radar::Trail trace{};
    if (got.status != 200 || radar::parse_trace(got.body.c_str(), got.body.size(), trace, radar::TRAIL_STEP_KM) <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> hold(radar::s_lock);
    std::snprintf(trace.hex, sizeof(trace.hex), "%s", hex);
    trace.seen_us         = std::time(nullptr);
    radar::s_trails[hex] = trace;
    return true;
}

/** The photo as RGB565, fitted inside the size the panel decodes to; empty for none. */
std::vector<std::uint16_t> fetch_photo(const char *hex, int &width, int &height)
{
    std::vector<std::uint16_t> pixels;
#if __APPLE__
    char url[128];
    std::snprintf(url, sizeof(url), "%s/pub/photos/hex/%s", radar::PHOTO_HOST, hex);
    const Answer lookup = get(url, {}, net::Priority::Tap);
    char         found[sizeof(radar::Details::photo_url)];
    if (lookup.status != 200 ||
        !radar::parse_photo(lookup.body.data(), lookup.body.size(), found, sizeof(found))) {
        return pixels;
    }
    const Answer image = get(found, {}, net::Priority::Tap);
    if (image.status != 200) {
        return pixels;
    }
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(image.body.data()),
                                  static_cast<CFIndex>(image.body.size()));
    CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
    CGImageRef       picture =
        source != nullptr ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    if (picture != nullptr) {
        const double scale =
            std::min(static_cast<double>(radar::PHOTO_MAX_W) / CGImageGetWidth(picture),
                     static_cast<double>(radar::PHOTO_MAX_H) / CGImageGetHeight(picture));
        width  = static_cast<int>(CGImageGetWidth(picture) * std::min(scale, 1.0));
        height = static_cast<int>(CGImageGetHeight(picture) * std::min(scale, 1.0));
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
        CGColorSpaceRef           space = CGColorSpaceCreateDeviceRGB();
        CGContextRef              context =
            CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space,
                                  kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), picture);
        pixels.resize(static_cast<std::size_t>(width) * height);
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const std::uint8_t *p = &rgba[i * 4];
            pixels[i] =
                static_cast<std::uint16_t>((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
        }
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        CGImageRelease(picture);
    }
    if (source != nullptr) {
        CFRelease(source);
    }
    CFRelease(data);
#else
    (void)hex, (void)width, (void)height;
#endif
    return pixels;
}
} // namespace live
