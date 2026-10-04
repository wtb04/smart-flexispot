#include "radar.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_state.h"
#include "jobs.h"
#include "jpeg.h"
#include "net.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

// The sky round home, and who is flying in it, through net: the feeds read on
// the planner's task every few seconds, and for a tapped aircraft its details,
// photo and trail asked for all at once, each answered on a net worker.
namespace radar {
namespace {
constexpr char TAG[] = "radar";

constexpr char PHOTO_AGENT[] = "smart-flexispot (+https://github.com/wtb04/smart-flexispot)";

// Past the last ring, since the fullscreen view shows the sky to the screen's
// edges, as far as 1.5 times the range from home at the widest.
constexpr int   RANGE_KM  = 250;
constexpr float NM_PER_KM = 0.539957f;

// The home position is snapped to this grid before it reaches a url or the
// screen, so neither carries the actual address.
constexpr float HOME_GRID_DEG = 0.01f;

constexpr int PHOTO_MAX_W = 320;
constexpr int PHOTO_MAX_H = 240;
// Decoded photos kept, so a plane tapped again shows at once; one of them is
// always the photo on screen, which the page draws straight from here.
constexpr int PHOTO_SLOTS   = 6;
constexpr int PHOTO_ENTRIES = 64;  // what is known of a photo: where it is, or that there is none

constexpr std::int64_t FIRST_FETCH_DELAY_US = 6 * units::kUsPerSecond;
constexpr int          HOME_SETTLE_CHECK_MS = units::kMsPerSecond;

// Enough for everything the feed has at its busiest, and some that have just
// left; one not heard of this long is forgotten, and its trail with it.
constexpr int          TRAIL_SLOTS     = 768;
constexpr float        TRAIL_STEP_KM   = 2.0f;  // with the points kept, 256 km: past the edge fullscreen
constexpr std::int64_t TRAIL_FORGET_US = 15 * units::kUsPerMinute;
// Past the slowest the feed is read, once a minute: unseen longer, a trail
// starts again.
constexpr std::int64_t TRAIL_GAP_US = 3 * units::kUsPerMinute;

// Measured: three seconds runs into the feed's rate limit and gets 429s.
constexpr std::int64_t POLL_ACTIVE_US = 5 * units::kUsPerSecond;
constexpr std::int64_t POLL_IDLE_US   = units::kUsPerMinute;
// An aircraft one feed has and the other not is kept through the other's
// turn, two readings apart while the radar shows: blinking out every other
// reading, it was chosen and dropped again each time.
constexpr std::int64_t KEEP_UNSEEN_US = 12 * units::kUsPerSecond;
constexpr int          PLANNER_REST_MS = units::kMsPerSecond;

constexpr std::size_t FEED_BODY_MAX    = 640 * units::kBytesPerKiB;  // uncompressed, about 940 bytes an aircraft
constexpr std::size_t LOOKUP_BODY_MAX  = 16 * units::kBytesPerKiB;
constexpr std::size_t PHOTO_BODY_MAX   = 256 * units::kBytesPerKiB;
constexpr std::size_t TRACE_BODY_MAX   = 64 * units::kBytesPerKiB;  // compressed
constexpr int         FEED_DEADLINE_MS = 10 * units::kMsPerSecond;
constexpr int         TAP_DEADLINE_MS  = 10 * units::kMsPerSecond;
constexpr std::size_t PATH_SIZE        = 160;

constexpr int HTTP_OK        = 200;
constexpr int HTTP_NOT_FOUND = 404;

constexpr int PREFETCH_PER_SWEEP = 6;
// Fewer than the cache holds: prefetching every aircraft in range evicted what
// the next sweep fetched again, forever.
constexpr int PREFETCH_NEAREST = 20;
constexpr int CACHE_SIZE       = 48;
static_assert(PREFETCH_NEAREST < CACHE_SIZE, "the prefetched set has to fit, or it churns");

jobs::Job s_job = jobs::kNoJob;  // it only plans: net does the fetching

// Everything below the lock is shared between the planner and net's workers.
SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_ctrl;
SemaphoreHandle_t s_publish_lock = nullptr;  // one snapshot handed on at a time
StaticSemaphore_t s_publish_lock_ctrl;

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

UpdateHandler  s_on_update  = nullptr;
DetailsHandler s_on_details = nullptr;
PhotoHandler   s_on_photo   = nullptr;

Aircraft    *s_list       = nullptr;
int          s_count      = 0;
Aircraft    *s_scratch    = nullptr;  // one feed is read at a time
Snapshot    *s_published  = nullptr;
bool         s_ok         = false;
std::int64_t s_fetched_us = 0;

bool         s_active  = false;
bool         s_enabled = true;
bool         s_screen  = true;  // nothing is fetched while the screen is dark
std::int64_t s_home_at_us = 0;
float        s_home_lat   = 0.0f;
float        s_home_lon   = 0.0f;
bool         s_has_home   = false;

// What was tapped last: only its answers reach the page.
std::atomic<std::uint32_t> s_tap{0};
char                       s_tap_hex[kHexLen]       = {};
char                       s_tap_flight[kFlightLen] = {};
std::int64_t               s_tap_at_us              = 0;
// The page asks on every redraw until the details are in; once asked, the
// same aircraft is not asked about again this soon.
constexpr std::int64_t TAP_REPEAT_US = 5 * units::kUsPerSecond;

int ms_since(std::int64_t began_us)
{
    return static_cast<int>((esp_timer_get_time() - began_us) / units::kUsPerMs);
}

bool tapped(std::uint32_t tap)
{
    return tap == s_tap.load();
}

// ---- hosts

// Two feeds of the same readsb data, taken in turn: one alone refused most
// readings at one every five seconds, the panel sharing its address with
// whatever else in the house asks.
struct Feed {
    const char *name;
    const char *query;  // lat, lon, radius in nautical miles
    net::Host   host;
};
Feed s_feeds[] = {
    {"adsb.lol", "/v2/point/%.4f/%.4f/%d", net::kNoHost},
    {"adsb.fi", "/api/v2/lat/%.4f/lon/%.4f/dist/%d", net::kNoHost},
};
constexpr int FEED_COUNT = static_cast<int>(std::size(s_feeds));

net::Host s_lookup_host   = net::kNoHost;  // adsbdb: the airframe and the flight
net::Host s_photoapi_host = net::kNoHost;  // planespotters: where the photo is
net::Host s_photo_host    = net::kNoHost;  // and the photo
net::Host s_trace_host    = net::kNoHost;  // adsb.lol's traces
net::Host s_hexdb_host    = net::kNoHost;  // hexdb.io: a route where adsbdb's does not fit

void add_hosts()
{
    // The two feeds stand in for each other, so one that fails is not tried
    // again but rested, and the other asked.
    net::HostConfig lol{};
    lol.name        = "adsb.lol";
    lol.base        = "http://api.adsb.lol";
    lol.timeout_ms  = 6 * units::kMsPerSecond;
    lol.connections = 1;
    lol.gzip        = true;
    lol.keep_open   = false;  // measured: holding it open doubled the share it refused
    lol.rest        = net::Rest{1, units::kMsPerMinute, units::kMsPerMinute};
    s_feeds[0].host = net::add_host(lol);

    net::HostConfig fi = lol;
    fi.name         = "adsb.fi";
    fi.base         = "https://opendata.adsb.fi";
    fi.keep_open    = true;  // https only, so held rather than shaking hands every reading
    s_feeds[1].host = net::add_host(fi);

    // Asked while somebody waits: tried once more shortly, and a moment's
    // failure rests them only briefly.
    net::HostConfig lookup{};
    lookup.name    = "adsbdb";
    lookup.base    = "https://api.adsbdb.com";
    lookup.idle_ms = units::kMsPerMinute;
    lookup.retry   = net::Retry{1, 300, 200, false};
    lookup.rest    = net::Rest{3, 10 * units::kMsPerSecond, 30 * units::kMsPerSecond};
    s_lookup_host  = net::add_host(lookup);

    net::HostConfig photoapi = lookup;
    photoapi.name   = "planespotters";
    photoapi.base   = "https://api.planespotters.net";
    photoapi.agent  = PHOTO_AGENT;
    s_photoapi_host = net::add_host(photoapi);

    net::HostConfig photo = lookup;
    photo.name   = "photos";
    photo.base   = "http://t.plnspttrs.net";
    s_photo_host = net::add_host(photo);

    net::HostConfig hexdb = lookup;
    hexdb.name    = "hexdb";
    hexdb.base    = "https://hexdb.io";
    s_hexdb_host  = net::add_host(hexdb);

    net::HostConfig trace = lookup;
    trace.name        = "adsb.lol traces";
    trace.base        = "https://adsb.lol";
    trace.timeout_ms  = 4 * units::kMsPerSecond;  // a trace only makes a trail longer
    trace.connections = 1;
    trace.retry       = net::Retry{};
    trace.rest        = net::Rest{2, units::kMsPerMinute, units::kMsPerMinute};
    s_trace_host      = net::add_host(trace);
}

// ---- publishing

void publish()
{
    if (s_on_update == nullptr) {
        return;
    }
    xSemaphoreTake(s_publish_lock, portMAX_DELAY);
    snapshot(*s_published);
    s_on_update(*s_published);
    xSemaphoreGive(s_publish_lock);
}

// ---- trails

Trail *s_trails = nullptr;

// Under the lock.
Trail *trail_slot(const char *hex, std::int64_t now)
{
    Trail *free_slot = nullptr;
    Trail *oldest    = nullptr;
    for (int i = 0; i < TRAIL_SLOTS; ++i) {
        Trail &slot = s_trails[i];
        if (slot.hex[0] != '\0' && std::strcmp(slot.hex, hex) == 0) {
            return &slot;
        }
        if (free_slot == nullptr && (slot.hex[0] == '\0' || now - slot.seen_us > TRAIL_FORGET_US)) {
            free_slot = &slot;
        }
        if (oldest == nullptr || slot.seen_us < oldest->seen_us) {
            oldest = &slot;
        }
    }
    Trail *slot = free_slot != nullptr ? free_slot : oldest;
    *slot       = Trail{};
    std::snprintf(slot->hex, sizeof(slot->hex), "%s", hex);
    return slot;
}

// Each reading adds to where those in it have been. Under the lock.
void record_trails(const Aircraft *list, int count)
{
    const std::int64_t now = esp_timer_get_time();
    for (int i = 0; i < count; ++i) {
        const Aircraft &aircraft = list[i];
        if (aircraft.on_ground || aircraft.hex[0] == '\0') {
            continue;
        }
        Trail *trail = trail_slot(aircraft.hex, now);
        seen(*trail, now, TRAIL_GAP_US);
        note(*trail, aircraft.lat, aircraft.lon, TRAIL_STEP_KM);
    }
}

// ---- details, cached

enum class Known : std::uint8_t { Empty, Asked, Known };

struct CacheEntry {
    char         hex[kHexLen];
    char         flight[kFlightLen];
    Details      details;
    std::int64_t used_us;
    Known        state;
};
CacheEntry *s_cache = nullptr;

// Under the lock.
CacheEntry *cache_find(const char *hex, const char *flight)
{
    for (int i = 0; i < CACHE_SIZE; ++i) {
        CacheEntry &entry = s_cache[i];
        if (entry.state != Known::Empty && std::strcmp(entry.hex, hex) == 0 && std::strcmp(entry.flight, flight) == 0) {
            entry.used_us = esp_timer_get_time();
            return &entry;
        }
    }
    return nullptr;
}

// Under the lock: the entry for `hex` on `flight`, taking the least used one
// for it when there is none.
CacheEntry &cache_slot(const char *hex, const char *flight)
{
    if (CacheEntry *found = cache_find(hex, flight); found != nullptr) {
        return *found;
    }
    CacheEntry *slot = &s_cache[0];
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (s_cache[i].state == Known::Empty) {
            slot = &s_cache[i];
            break;
        }
        if (s_cache[i].used_us < slot->used_us) {
            slot = &s_cache[i];
        }
    }
    *slot = CacheEntry{};
    std::snprintf(slot->hex, sizeof(slot->hex), "%s", hex);
    std::snprintf(slot->flight, sizeof(slot->flight), "%s", flight);
    slot->used_us = esp_timer_get_time();
    return *slot;
}

// Under the lock: known details of those no longer in the feed dropped.
void expire_cache()
{
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (s_cache[i].state != Known::Known) {
            continue;
        }
        bool still_here = false;
        for (int j = 0; j < s_count && !still_here; ++j) {
            still_here = std::strcmp(s_cache[i].hex, s_list[j].hex) == 0;
        }
        if (!still_here) {
            s_cache[i].state = Known::Empty;
        }
    }
}

// A lookup of the airframe and the flight it is on, in as many requests as it
// takes. One answers for both -- but it is all or nothing: an unknown callsign
// 404s the whole thing, and the body says which half was missing, so only
// that half is asked for again.
struct DetailsJob {
    char          hex[kHexLen];
    char          flight[kFlightLen];
    Details       details{};
    net::Priority priority;
    std::uint32_t tap;  // 0 when prefetched
    bool          want_aircraft = false;
    bool          want_route    = false;
    bool          tried_hexdb   = false;  // asked for another route, as adsbdb's did not fit
    char          ends[2][kAirportCodeLen] = {};  // hexdb's route, as ICAO codes
    Airport       airports[2]   = {};
    bool          failed        = false;
    std::int64_t  began_us      = 0;
};
using DetailsJobPtr = std::shared_ptr<DetailsJob>;

void details_step(const DetailsJobPtr &job);

// A route the database has for the callsign that does not fit where the
// aircraft is: another of the callsign's days, shown as no route rather than
// as the wrong one; one it fits but flies the other way, turned round; and
// the leg either side of it, as the one airport of it that is known.
// Under the lock.
void drop_route_unless_fits(Details &details, const char *hex)
{
    for (int i = 0; i < s_count; ++i) {
        if (std::strcmp(s_list[i].hex, hex) != 0) {
            continue;
        }
        char from[kAirportCodeLen];
        char to[kAirportCodeLen];
        std::memcpy(from, details.origin_code, sizeof(from));
        std::memcpy(to, details.dest_code, sizeof(to));
        switch (judge_route(details, s_list[i].lat, s_list[i].lon, s_list[i].track_deg)) {
        case RouteVerdict::IntoOrigin:
            ESP_LOGI(TAG, "%s: %s to %s, on the leg into %s", hex, from, to, from);
            break;
        case RouteVerdict::OutOfDest:
            ESP_LOGI(TAG, "%s: %s to %s, on the leg out of %s", hex, from, to, to);
            break;
        case RouteVerdict::Dropped:
            ESP_LOGI(TAG, "%s: %s to %s does not fit where it is, left out", hex, from, to);
            break;
        case RouteVerdict::Reversed:
            ESP_LOGI(TAG, "%s: %s to %s flown the other way round", hex, from, to);
            break;
        case RouteVerdict::Kept:
            break;
        }
        return;
    }
}

void details_done(const DetailsJobPtr &job)
{
    {
        Lock        hold;
        if (!job->failed) {
            drop_route_unless_fits(job->details, job->hex);
        }
        CacheEntry &entry = cache_slot(job->hex, job->flight);
        if (job->failed) {
            entry.state = Known::Empty;  // asked again next time rather than known as nothing
        } else {
            entry.details = job->details;
            entry.state   = Known::Known;
        }
    }
    ESP_LOGI(TAG, "%s: %s%s%s in %d ms", job->hex, job->details.has_aircraft ? job->details.model : "unknown type",
             job->details.has_route ? ", " : "", job->details.has_route ? job->details.origin_code : "",
             ms_since(job->began_us));
    if (job->tap != 0 && tapped(job->tap) && s_on_details != nullptr) {
        s_on_details(job->hex, job->details);
    }
}

void ask_lookup(const DetailsJobPtr &job, const char *path, const char *what, net::Done done,
                net::Host host = s_lookup_host)
{
    net::Request request;
    request.host        = host;
    request.path        = path;
    request.priority    = job->priority;
    // Keyed by the aircraft, not by who asks: a tap joins a prefetch already
    // under way, at a tap's priority, rather than ask again.
    request.key         = std::string("details:") + job->hex + ":" + what;
    request.dedupe      = net::Dedupe::Join;
    request.deadline_ms = job->tap != 0 ? TAP_DEADLINE_MS : 0;
    request.max_body    = LOOKUP_BODY_MAX;
    request.what        = what;
    request.done        = std::move(done);
    net::submit(std::move(request));
}

// The airports hexdb.io has named, kept for the next route through them.
constexpr int AIRPORTS_KEPT = 32;
struct KeptAirport {
    char    icao[kAirportCodeLen];
    Airport airport;
};
KeptAirport s_airports[AIRPORTS_KEPT] = {};
int         s_airports_next          = 0;

// Whether the route adsbdb gave, if any, fits where the aircraft is now.
bool route_holds(const DetailsJobPtr &job)
{
    Lock hold;
    for (int i = 0; i < s_count; ++i) {
        if (std::strcmp(s_list[i].hex, job->hex) == 0) {
            return job->details.has_route && route_fits(job->details, s_list[i].lat, s_list[i].lon);
        }
    }
    return true;  // gone from view: nothing to judge it by
}

// Both of hexdb's airports in hand: its route takes adsbdb's place, to be
// judged as that one was.
void take_hexdb_route(const DetailsJobPtr &job)
{
    Details &d = job->details;
    const Airport &from = job->airports[0];
    const Airport &to   = job->airports[1];
    std::snprintf(d.origin_code, sizeof(d.origin_code), "%s", from.code[0] != '\0' ? from.code : job->ends[0]);
    std::snprintf(d.origin_city, sizeof(d.origin_city), "%s", from.name);
    std::snprintf(d.dest_code, sizeof(d.dest_code), "%s", to.code[0] != '\0' ? to.code : job->ends[1]);
    std::snprintf(d.dest_city, sizeof(d.dest_city), "%s", to.name);
    d.origin_lat = from.lat, d.origin_lon = from.lon, d.dest_lat = to.lat, d.dest_lon = to.lon;
    d.has_origin_at = d.has_dest_at = d.has_route = true;
    details_step(job);
}

void ask_hexdb_airport(const DetailsJobPtr &job, int which)
{
    if (which == 2) {
        take_hexdb_route(job);
        return;
    }
    bool kept_here = false;
    {
        Lock hold;
        for (const KeptAirport &kept : s_airports) {
            if (std::strcmp(kept.icao, job->ends[which]) == 0) {
                job->airports[which] = kept.airport;
                kept_here            = true;
                break;
            }
        }
    }
    if (kept_here) {  // on from here, outside the lock, which the next steps take
        ask_hexdb_airport(job, which + 1);
        return;
    }
    char path[PATH_SIZE];
    std::snprintf(path, sizeof(path), "/api/v1/airport/icao/%s", job->ends[which]);
    ask_lookup(job, path, which == 0 ? "origin" : "destination", [job, which](const net::Response &answer) {
        if (answer.status != HTTP_OK || !parse_airport(answer.body, answer.length, job->airports[which])) {
            details_step(job);  // without it, adsbdb's route is judged as it was
            return;
        }
        {
            Lock         hold;
            KeptAirport &kept = s_airports[s_airports_next];
            s_airports_next   = (s_airports_next + 1) % AIRPORTS_KEPT;
            std::snprintf(kept.icao, sizeof(kept.icao), "%s", job->ends[which]);
            kept.airport = job->airports[which];
        }
        ask_hexdb_airport(job, which + 1);
    }, s_hexdb_host);
}

// adsbdb's route for the callsign is often another leg of a low-cost airline's
// day: hexdb.io keeps routes of its own, which fit where adsbdb's do not.
void ask_hexdb_route(const DetailsJobPtr &job)
{
    char path[PATH_SIZE];
    std::snprintf(path, sizeof(path), "/callsign-route?callsign=%s", job->flight);
    ask_lookup(job, path, "other route", [job](const net::Response &answer) {
        if (answer.status == HTTP_OK &&
            parse_route_codes(answer.body, answer.length, job->ends[0], job->ends[1], sizeof(job->ends[0]))) {
            ask_hexdb_airport(job, 0);
        } else {
            details_step(job);
        }
    }, s_hexdb_host);
}

void details_step(const DetailsJobPtr &job)
{
    char path[PATH_SIZE];
    if (job->want_aircraft && job->want_route) {
        std::snprintf(path, sizeof(path), "/v0/aircraft/%s?callsign=%s", job->hex, job->flight);
        ask_lookup(job, path, "details", [job](const net::Response &answer) {
            if (answer.status == HTTP_OK) {
                parse_aircraft(answer.body, answer.length, job->details);
                parse_route(answer.body, answer.length, job->details);
                job->want_aircraft = job->want_route = false;
            } else if (answer.status == HTTP_NOT_FOUND) {
                job->want_route    = std::strstr(answer.body, "unknown callsign") == nullptr;
                job->want_aircraft = std::strstr(answer.body, "unknown aircraft") == nullptr;
            } else {
                job->failed        = true;
                job->want_aircraft = job->want_route = false;
            }
            details_step(job);
        });
    } else if (job->want_aircraft) {
        std::snprintf(path, sizeof(path), "/v0/aircraft/%s", job->hex);
        ask_lookup(job, path, "aircraft", [job](const net::Response &answer) {
            if (answer.status == HTTP_OK) {
                parse_aircraft(answer.body, answer.length, job->details);
            }
            job->want_aircraft = false;
            details_step(job);
        });
    } else if (job->want_route) {
        std::snprintf(path, sizeof(path), "/v0/callsign/%s", job->flight);
        ask_lookup(job, path, "route", [job](const net::Response &answer) {
            if (answer.status == HTTP_OK) {
                parse_route(answer.body, answer.length, job->details);
            }
            job->want_route = false;
            details_step(job);
        });
    } else if (!job->tried_hexdb && !job->failed && job->flight[0] != '\0' && !route_holds(job)) {
        job->tried_hexdb = true;
        ask_hexdb_route(job);
    } else {
        details_done(job);
    }
}

void ask_details(const char *hex, const char *flight, net::Priority priority, std::uint32_t tap)
{
    auto job = std::make_shared<DetailsJob>();
    std::snprintf(job->hex, sizeof(job->hex), "%s", hex);
    std::snprintf(job->flight, sizeof(job->flight), "%s", flight);
    job->priority      = priority;
    job->tap           = tap;
    job->want_aircraft = true;
    job->want_route    = flight[0] != '\0';
    job->began_us      = esp_timer_get_time();
    details_step(job);
}

// ---- photos, cached

enum class Photo : std::uint8_t { Unknown, Asked, None, Found };

struct PhotoEntry {
    char         hex[kHexLen];
    char         url[kPhotoUrlLen];
    char         credit[kPhotographerLen];
    Photo        state;
    int          slot;  // decoded into, or -1
    std::int64_t used_us;
};
struct PhotoSlot {
    std::uint16_t *pixels;
    int            width;
    int            height;
    int            entry;  // whose it is, or -1
    bool           filling;
};
PhotoEntry *s_photo_entries = nullptr;
PhotoSlot   s_photo_slots[PHOTO_SLOTS];
int         s_shown_slot = -1;  // the page draws straight from it, so it is never reused

// Under the lock: the entry for `hex`, taking the least used one when there is none.
int photo_entry(const char *hex)
{
    int least = 0;
    for (int i = 0; i < PHOTO_ENTRIES; ++i) {
        PhotoEntry &entry = s_photo_entries[i];
        if (entry.hex[0] != '\0' && std::strcmp(entry.hex, hex) == 0) {
            entry.used_us = esp_timer_get_time();
            return i;
        }
        const bool holds_shown = entry.slot >= 0 && entry.slot == s_shown_slot;
        if (!holds_shown && entry.used_us < s_photo_entries[least].used_us) {
            least = i;
        }
    }
    PhotoEntry &entry = s_photo_entries[least];
    if (entry.slot >= 0) {
        s_photo_slots[entry.slot].entry = -1;
    }
    entry = PhotoEntry{};
    std::snprintf(entry.hex, sizeof(entry.hex), "%s", hex);
    entry.slot    = -1;
    entry.used_us = esp_timer_get_time();
    return least;
}

// Under the lock: a slot to decode into, taken from the least used photo
// that is not on screen; -1 when every one is busy.
int photo_slot_for(int entry)
{
    int chosen = -1;
    for (int i = 0; i < PHOTO_SLOTS; ++i) {
        const PhotoSlot &slot = s_photo_slots[i];
        if (slot.filling || i == s_shown_slot) {
            continue;
        }
        if (slot.entry < 0) {
            chosen = i;
            break;
        }
        if (chosen < 0 ||
            s_photo_entries[slot.entry].used_us < s_photo_entries[s_photo_slots[chosen].entry].used_us) {
            chosen = i;
        }
    }
    if (chosen >= 0) {
        PhotoSlot &slot = s_photo_slots[chosen];
        if (slot.entry >= 0) {
            s_photo_entries[slot.entry].slot = -1;
        }
        slot.entry   = entry;
        slot.filling = true;
    }
    return chosen;
}

void show_photo(const char *hex, std::uint32_t tap, int slot)
{
    if (!tapped(tap) || s_on_photo == nullptr) {
        return;
    }
    if (slot < 0) {
        s_on_photo(hex, nullptr, 0, 0, "");
        return;
    }
    char credit[kPhotographerLen] = "";
    {
        Lock hold;
        std::snprintf(credit, sizeof(credit), "%s", s_photo_entries[s_photo_slots[slot].entry].credit);
    }
    const PhotoSlot &photo = s_photo_slots[slot];
    s_on_photo(hex, photo.pixels, photo.width, photo.height, credit);
}

void ask_image(const std::string &hex, const std::string &url, std::uint32_t tap)
{
    net::Request request;
    request.host        = s_photo_host;
    request.path        = url;
    request.priority    = net::Priority::Tap;
    request.key         = "photo";
    request.dedupe      = net::Dedupe::Replace;
    request.deadline_ms = TAP_DEADLINE_MS;
    request.max_body    = PHOTO_BODY_MAX;
    request.what        = "photo";
    request.done        = [hex, tap](const net::Response &answer) {
        int slot = -1;
        {
            Lock hold;
            const int entry = photo_entry(hex.c_str());
            if (answer.status == HTTP_OK) {
                slot = photo_slot_for(entry);
            }
        }
        if (slot < 0) {
            show_photo(hex.c_str(), tap, -1);
            return;
        }
        PhotoSlot &photo = s_photo_slots[slot];
        int        width = 0, height = 0;
        const bool decoded =
            jpeg::decode_into(answer.body, answer.length, photo.pixels, PHOTO_MAX_W, PHOTO_MAX_H, width, height);
        {
            Lock hold;
            const int entry = photo_entry(hex.c_str());
            photo.filling   = false;
            if (decoded) {
                photo.width                   = width;
                photo.height                  = height;
                s_photo_entries[entry].slot   = slot;
                if (tapped(tap)) {
                    s_shown_slot = slot;
                }
            } else {
                photo.entry = -1;
                slot        = -1;
            }
        }
        if (decoded) {
            ESP_LOGI(TAG, "%s: photo %dx%d in %d ms", hex.c_str(), width, height, answer.ms);
        }
        show_photo(hex.c_str(), tap, slot);
    };
    net::submit(std::move(request));
}

// Where the photo is, from planespotters; then, for a tap, the photo.
void ask_photo_lookup(const char *hex, net::Priority priority, std::uint32_t tap)
{
    char path[PATH_SIZE];
    std::snprintf(path, sizeof(path), "/pub/photos/hex/%s", hex);
    net::Request request;
    request.host        = s_photoapi_host;
    request.path        = path;
    request.priority    = priority;
    request.key         = std::string("photo lookup:") + hex;
    request.dedupe      = net::Dedupe::Join;
    request.deadline_ms = tap != 0 ? TAP_DEADLINE_MS : 0;
    request.max_body    = LOOKUP_BODY_MAX;
    request.what        = "photo lookup";
    request.done        = [hex = std::string(hex), tap](const net::Response &answer) {
        char found[kPhotoUrlLen]       = "";
        char credit[kPhotographerLen] = "";
        const bool answered = answer.status == HTTP_OK || answer.status == HTTP_NOT_FOUND;
        const bool has      = answer.status == HTTP_OK &&
                         parse_photo(answer.body, answer.length, found, sizeof(found), credit, sizeof(credit));
        std::string url;
        if (has) {
            // Over plain http: the image host takes it, and it saves a handshake.
            url = std::strncmp(found, "https://", 8) == 0 ? std::string("http://") + (found + 8) : found;
        }
        {
            Lock        hold;
            PhotoEntry &entry = s_photo_entries[photo_entry(hex.c_str())];
            entry.state       = has ? Photo::Found : answered ? Photo::None : Photo::Unknown;
            std::snprintf(entry.url, sizeof(entry.url), "%s", url.c_str());
            std::snprintf(entry.credit, sizeof(entry.credit), "%s", credit);
        }
        if (tap == 0 || !tapped(tap)) {
            return;
        }
        if (has) {
            ask_image(hex, url, tap);
        } else {
            show_photo(hex.c_str(), tap, -1);
        }
    };
    net::submit(std::move(request));
}

void photo_for_tap(const char *hex, std::uint32_t tap)
{
    int         slot = -1;
    Photo       state;
    std::string url;
    {
        Lock        hold;
        PhotoEntry &entry = s_photo_entries[photo_entry(hex)];
        state             = entry.state;
        slot              = entry.slot;
        url               = entry.url;
        if (slot >= 0) {
            s_shown_slot = slot;
        }
    }
    if (slot >= 0 || state == Photo::None) {
        show_photo(hex, tap, slot);
    } else if (state == Photo::Found) {
        ask_image(hex, url, tap);
    } else {
        ask_photo_lookup(hex, net::Priority::Tap, tap);
    }
}

// ---- traces

// Where a tapped aircraft has been this last quarter hour, before the panel
// was watching, so its trail reaches back across the view at once.
void ask_trace(const char *hex)
{
    const std::size_t len = std::strlen(hex);
    if (len < 2 || hex[0] == '~') {
        return;  // a ~ is a position from ground radar, which has no trace
    }
    char path[PATH_SIZE];
    std::snprintf(path, sizeof(path), "/data/traces/%.2s/trace_recent_%.7s.json", hex + len - 2, hex);
    net::Request request;
    request.host        = s_trace_host;
    request.path        = path;
    request.priority    = net::Priority::Now;
    request.key         = "trace";
    request.dedupe      = net::Dedupe::Replace;
    request.deadline_ms = TAP_DEADLINE_MS;
    request.max_body    = TRACE_BODY_MAX;
    request.what        = "trace";
    request.done        = [hex = std::string(hex)](const net::Response &answer) {
        if (answer.status != HTTP_OK) {
            return;
        }
        auto      trace     = std::make_unique<Trail>();
        const int positions = parse_trace(answer.body, answer.length, *trace, TRAIL_STEP_KM);
        if (positions <= 0) {
            ESP_LOGW(TAG, "%s: no trace in %u bytes", hex.c_str(), static_cast<unsigned>(answer.length));
            return;
        }
        ESP_LOGI(TAG, "%s: trace, %d positions in %d ms", hex.c_str(), positions, answer.ms);
        {
            Lock               hold;
            const std::int64_t now  = esp_timer_get_time();
            Trail             *slot = trail_slot(hex.c_str(), now);
            std::memcpy(trace->hex, slot->hex, sizeof(trace->hex));
            trace->seen_us = now;
            *slot          = *trace;
        }
        publish();
    };
    net::submit(std::move(request));
}

// ---- prefetch

struct Want {
    char  hex[kHexLen];
    char  flight[kFlightLen];
    float distance;
};
Want *s_wanted = nullptr;  // PSRAM, as the lists it is taken from are

// The nearest aircraft's details and where their photos are, asked for in the
// background: a tap on one then waits only for the photo itself.
void prefetch()
{
    int count = 0;
    {
        Lock hold;
        for (int i = 0; i < s_count; ++i) {
            const Aircraft &aircraft = s_list[i];
            if (aircraft.on_ground || aircraft.hex[0] == '\0') {
                continue;
            }
            std::memcpy(s_wanted[count].hex, aircraft.hex, sizeof(s_wanted[count].hex));
            std::memcpy(s_wanted[count].flight, aircraft.flight, sizeof(s_wanted[count].flight));
            s_wanted[count].distance = aircraft.distance_nm;
            ++count;
        }
    }
    std::sort(s_wanted, s_wanted + count, [](const Want &a, const Want &b) { return a.distance < b.distance; });

    int details = 0, photos = 0;
    for (int i = 0; i < count && i < PREFETCH_NEAREST; ++i) {
        const Want &want      = s_wanted[i];
        bool        ask_about = false, ask_where = false;
        {
            Lock hold;
            if (details < PREFETCH_PER_SWEEP && cache_find(want.hex, want.flight) == nullptr) {
                cache_slot(want.hex, want.flight).state = Known::Asked;  // not asked twice while it waits
                ask_about = true;
                ++details;
            }
            PhotoEntry &entry = s_photo_entries[photo_entry(want.hex)];
            if (photos < PREFETCH_PER_SWEEP && entry.state == Photo::Unknown) {
                entry.state = Photo::Asked;
                ask_where   = true;
                ++photos;
            }
        }
        if (ask_about) {
            ask_details(want.hex, want.flight, net::Priority::Background, 0);
        }
        if (ask_where) {
            ask_photo_lookup(want.hex, net::Priority::Background, 0);
        }
    }
}

// ---- feeds

std::atomic<bool> s_feed_out{false};  // a reading asked for and not yet in
int               s_next_feed = 0;

void feed_failed()
{
    {
        Lock hold;
        s_ok = false;
    }
    s_feed_out.store(false);
    publish();
}

void ask_feed(int feed, int tried, float lat, float lon);

void take_feed(const net::Response &answer, int feed, int tried, float lat, float lon, bool active)
{
    if (answer.status != HTTP_OK) {
        // The other at once, unless it has been tried already.
        const int other = (feed + 1) % FEED_COUNT;
        if ((tried & (1 << other)) == 0) {
            ask_feed(other, tried, lat, lon);
        } else {
            feed_failed();
        }
        return;
    }
    int count = parse(answer.body, answer.length, s_scratch, kMaxAircraft);
    {
        Lock hold;
        count = merge_reading(s_scratch, count, kMaxAircraft, s_list, s_count, esp_timer_get_time(), KEEP_UNSEEN_US);
        std::memcpy(s_list, s_scratch, sizeof(Aircraft) * static_cast<std::size_t>(count));
        record_trails(s_list, count);
        s_count      = count;
        s_ok         = true;
        s_fetched_us = esp_timer_get_time();
        expire_cache();
    }
    ESP_LOGD(TAG, "%s: %d aircraft within %d km, %u bytes in %d ms", s_feeds[feed].name, count, RANGE_KM,
             static_cast<unsigned>(answer.length), answer.ms);
    s_feed_out.store(false);
    publish();
    if (active) {
        prefetch();
    }
}

void ask_feed(int feed, int tried, float lat, float lon)
{
    // Past one that is resting, when the other is not.
    const int other = (feed + 1) % FEED_COUNT;
    if (net::resting(s_feeds[feed].host) && (tried & (1 << other)) == 0 && !net::resting(s_feeds[other].host)) {
        feed = other;
    }
    tried |= 1 << feed;
    bool active = false;
    {
        Lock hold;
        active = s_active;
    }
    char path[PATH_SIZE];
    std::snprintf(path, sizeof(path), s_feeds[feed].query, static_cast<double>(lat), static_cast<double>(lon),
                  static_cast<int>(std::lround(static_cast<float>(RANGE_KM) * NM_PER_KM)));
    net::Request request;
    request.host        = s_feeds[feed].host;
    request.path        = path;
    request.priority    = net::Priority::Now;
    request.key         = "feed";
    request.dedupe      = net::Dedupe::Replace;
    request.deadline_ms = FEED_DEADLINE_MS;
    request.max_body    = FEED_BODY_MAX;
    request.what        = "feed";  // not the path: it carries the panel's own coordinates
    request.done        = [feed, tried, lat, lon, active](const net::Response &answer) {
        take_feed(answer, feed, tried, lat, lon, active);
    };
    net::submit(std::move(request));
}

// Reads the feed when it is due; the rest happens on net's workers.
jobs::Result plan()
{
    static std::int64_t last_fetch = 0;
    bool                ready = false, active = false;
    float               lat = 0.0f, lon = 0.0f;
    std::int64_t        home_at = 0;
    {
        Lock hold;
        ready   = s_has_home && s_enabled && s_screen;
        active  = s_active;
        lat     = s_home_lat;
        lon     = s_home_lon;
        home_at = s_home_at_us;
    }
    if (!ready) {
        return jobs::sleep();
    }
    const std::int64_t now = esp_timer_get_time();
    if (last_fetch == 0 && now - home_at < FIRST_FETCH_DELAY_US) {
        return jobs::again_in(HOME_SETTLE_CHECK_MS);
    }
    const std::int64_t due = active ? POLL_ACTIVE_US : POLL_IDLE_US;
    if (!s_feed_out.load() && (last_fetch == 0 || now - last_fetch >= due)) {
        last_fetch = now;
        s_feed_out.store(true);
        const int feed = s_next_feed;
        s_next_feed    = (s_next_feed + 1) % FEED_COUNT;
        ask_feed(feed, 0, lat, lon);
    }
    return jobs::again_in(PLANNER_REST_MS);
}

void wake()
{
    jobs::poke(s_job);
}

// Nothing is fetched while the screen is dark; lit, it fetches at once.
void set_screen(bool on)
{
    bool woke = false;
    {
        Lock hold;
        woke     = on && !s_screen;
        s_screen = on;
    }
    if (woke) {
        wake();
    }
}

// Call with the lock held.
int age_s()
{
    return s_fetched_us == 0 ? -1 : static_cast<int>((esp_timer_get_time() - s_fetched_us) / units::kUsPerSecond);
}

template <typename T>
T *psram_calloc(std::size_t count)
{
    return static_cast<T *>(heap_caps_calloc(count, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

}  // namespace

esp_err_t start(UpdateHandler on_update, DetailsHandler on_details, PhotoHandler on_photo)
{
    s_on_update  = on_update;
    s_on_details = on_details;
    s_on_photo   = on_photo;

    s_lock         = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    s_publish_lock = xSemaphoreCreateMutexStatic(&s_publish_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr && s_publish_lock != nullptr, ESP_ERR_NO_MEM, TAG, "locks");

    s_list          = psram_calloc<Aircraft>(kMaxAircraft);
    s_scratch       = psram_calloc<Aircraft>(kMaxAircraft);
    s_published     = psram_calloc<Snapshot>(1);
    s_wanted        = psram_calloc<Want>(kMaxAircraft);
    s_trails        = psram_calloc<Trail>(TRAIL_SLOTS);
    s_cache         = psram_calloc<CacheEntry>(CACHE_SIZE);
    s_photo_entries = psram_calloc<PhotoEntry>(PHOTO_ENTRIES);
    ESP_RETURN_ON_FALSE(s_list != nullptr && s_scratch != nullptr && s_published != nullptr && s_wanted != nullptr &&
                            s_trails != nullptr && s_cache != nullptr && s_photo_entries != nullptr,
                        ESP_ERR_NO_MEM, TAG, "buffers");
    for (int i = 0; i < PHOTO_ENTRIES; ++i) {
        s_photo_entries[i].slot = -1;
    }
    for (PhotoSlot &slot : s_photo_slots) {
        slot = {psram_calloc<std::uint16_t>(static_cast<std::size_t>(PHOTO_MAX_W) * PHOTO_MAX_H), 0, 0, -1, false};
        ESP_RETURN_ON_FALSE(slot.pixels != nullptr, ESP_ERR_NO_MEM, TAG, "photo buffers");
    }

    add_hosts();
    s_screen = app::get(app::Fact::ScreenOn);
    app::watch(app::Fact::ScreenOn, set_screen);
    jobs::Spec spec;
    spec.name = TAG;
    spec.run  = plan;
    s_job     = jobs::add(std::move(spec));
    ESP_RETURN_ON_FALSE(s_job != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

void set_active(bool active)
{
    if (s_lock == nullptr) {
        return;
    }
    bool woke = false;
    {
        Lock hold;
        woke     = active && !s_active;
        s_active = active;
    }
    if (woke) {
        wake();
    }
}

void request_details(const char *hex, const char *callsign)
{
    if (s_lock == nullptr || hex == nullptr) {
        return;
    }
    const char        *flight = callsign != nullptr ? callsign : "";
    const std::int64_t now    = esp_timer_get_time();
    std::uint32_t      tap    = 0;
    bool               known  = false;
    Details            details{};
    {
        Lock hold;
        if (std::strcmp(s_tap_hex, hex) == 0 && std::strcmp(s_tap_flight, flight) == 0 &&
            now - s_tap_at_us < TAP_REPEAT_US) {
            return;  // already on its way
        }
        tap = s_tap.fetch_add(1) + 1;
        if (tap == 0) {
            tap = s_tap.fetch_add(1) + 1;  // 0 is a prefetch's
        }
        std::snprintf(s_tap_hex, sizeof(s_tap_hex), "%s", hex);
        std::snprintf(s_tap_flight, sizeof(s_tap_flight), "%s", flight);
        s_tap_at_us = now;
        if (const CacheEntry *entry = cache_find(hex, flight); entry != nullptr && entry->state == Known::Known) {
            known   = true;
            details = entry->details;
        }
    }
    // All three at once: none needs what another brings.
    if (known) {
        if (s_on_details != nullptr) {
            s_on_details(hex, details);
        }
    } else {
        ask_details(hex, flight, net::Priority::Tap, tap);
    }
    photo_for_tap(hex, tap);
    ask_trace(hex);
}


void set_enabled(bool enabled)
{
    if (s_lock == nullptr) {
        return;
    }
    bool woke = false;
    {
        Lock hold;
        woke      = enabled && !s_enabled;
        s_enabled = enabled;
    }
    if (woke) {
        wake();
    }
}

void set_home(float lat, float lon)
{
    if (s_lock == nullptr) {
        return;
    }
    bool first = false;
    {
        Lock hold;
        first = !s_has_home;
        if (first) {
            s_home_at_us = esp_timer_get_time();
        }
        s_home_lat = std::round(lat / HOME_GRID_DEG) * HOME_GRID_DEG;
        s_home_lon = std::round(lon / HOME_GRID_DEG) * HOME_GRID_DEG;
        s_has_home = true;
    }
    if (first) {
        ESP_LOGI(TAG, "centred on Home Assistant's home zone");
        wake();
    }
}

int trail(const char *hex, TrailPoint *out, int max)
{
    if (s_lock == nullptr || s_trails == nullptr || hex == nullptr || hex[0] == '\0') {
        return 0;
    }
    Lock hold;
    for (int i = 0; i < TRAIL_SLOTS; ++i) {
        if (std::strcmp(s_trails[i].hex, hex) == 0) {
            return oldest_first(s_trails[i], out, max);
        }
    }
    return 0;
}

void snapshot(Snapshot &out)
{
    if (s_lock == nullptr) {
        out       = Snapshot{};
        out.age_s = -1;
        return;
    }
    Lock hold;
    std::memcpy(out.list, s_list, sizeof(Aircraft) * static_cast<std::size_t>(s_count));
    out.count    = s_count;
    out.home_lat = s_home_lat;
    out.home_lon = s_home_lon;
    out.range_km = RANGE_KM;
    out.ok       = s_ok;
    out.age_s    = age_s();
}

void status(Status &out)
{
    if (s_lock == nullptr) {
        out = Status{0, RANGE_KM, false, -1};
        return;
    }
    Lock hold;
    out.count    = s_count;
    out.range_km = RANGE_KM;
    out.ok       = s_ok;
    out.age_s    = age_s();
}

}  // namespace radar
