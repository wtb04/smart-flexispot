#include "radar.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg.h"
#include "miniz.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "units.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace radar {
namespace {
constexpr char TAG[] = "radar";

constexpr char LOOKUP_HOST[]      = "https://api.adsbdb.com";
constexpr char PHOTO_HOST[]       = "https://api.planespotters.net";
constexpr char PHOTO_IMAGE_HOST[] = "http://t.plnspttrs.net";
constexpr char TRACE_HOST[]       = "https://adsb.lol";
constexpr char PHOTO_AGENT[]      = "smart-flexispot (+https://woutertenbrinke.nl)";
constexpr char AGENT[]            = "smart-flexispot";

constexpr char        HTTPS_PREFIX[]   = "https://";
constexpr std::size_t HTTPS_PREFIX_LEN = sizeof(HTTPS_PREFIX) - 1;

constexpr int HTTP_TIMEOUT_MS  = 10 * units::kMsPerSecond;
constexpr int HTTP_BUFFER_SIZE = 2 * units::kBytesPerKiB;

constexpr int HTTP_OK                = 200;
constexpr int HTTP_NOT_FOUND         = 404;
constexpr int HTTP_TOO_MANY_REQUESTS = 429;

constexpr std::size_t URL_SIZE        = 128;
constexpr std::size_t LOOKUP_URL_SIZE = 192;

// Past the last ring, since the fullscreen view shows the sky to the screen's
// edges, as far as 1.5 times the range from home at the widest.
constexpr int   RANGE_KM  = 250;
constexpr float NM_PER_KM = 0.539957f;

// The home position is snapped to this grid before it reaches a url or the
// screen, so neither carries the actual address.
constexpr float HOME_GRID_DEG = 0.01f;

constexpr int PHOTO_MAX_W = 320;
constexpr int PHOTO_MAX_H = 240;

constexpr std::int64_t FIRST_FETCH_DELAY_US = 6 * units::kUsPerSecond;
constexpr TickType_t   HOME_SETTLE_CHECK    = pdMS_TO_TICKS(units::kMsPerSecond);

// Measured: three seconds runs into the feed's rate limit and gets 429s.
// Enough for everything the feed has at its busiest, and some that have just
// left; one not heard of this long is forgotten, and its trail with it.
constexpr int          TRAIL_SLOTS    = 768;
constexpr float        TRAIL_STEP_KM  = 2.0f;  // with the points kept, 256 km: past the edge fullscreen
constexpr std::int64_t TRAIL_FORGET_US = 15 * units::kUsPerMinute;

constexpr std::int64_t POLL_ACTIVE_US = 5 * units::kUsPerSecond;
constexpr std::int64_t POLL_IDLE_US   = units::kUsPerMinute;

constexpr std::int64_t OVERDUE_REST_MS = units::kMsPerSecond;
// Short enough to close idle lookup connections on time.
constexpr std::int64_t LOOKUPS_OPEN_REST_MS = 2 * units::kMsPerSecond;

constexpr std::size_t BODY_MAX = 640 * units::kBytesPerKiB;  // about 940 bytes an aircraft

constexpr std::uint32_t TASK_STACK    = 8192;  // measured: uses 3.1 KB; the TLS handshake runs on it
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

char        *s_body     = nullptr;
std::size_t  s_body_len = 0;

constexpr std::int64_t IDLE_CLOSE_US = 20 * units::kUsPerSecond;
constexpr std::int64_t PHOTO_IDLE_US = 30 * units::kUsPerSecond;

esp_http_client_handle_t s_lookup_client   = nullptr;
esp_http_client_handle_t s_photoapi_client = nullptr;
esp_http_client_handle_t s_photo_client    = nullptr;
esp_http_client_handle_t s_trace_client    = nullptr;

std::int64_t s_lookup_at_us = 0;
bool         s_lookups_open = false;
std::int64_t s_photo_at_us  = 0;
bool         s_photo_open   = false;

constexpr int PREFETCH_PER_SWEEP = 6;
// Fewer than the cache holds: prefetching every aircraft in range evicted what
// the next sweep fetched again, forever.
constexpr int PREFETCH_NEAREST = 20;

constexpr int CACHE_SIZE = 48;
static_assert(PREFETCH_NEAREST < CACHE_SIZE, "the prefetched set has to fit, or it churns");

struct CacheEntry {
    char         hex[kHexLen];
    char         flight[kFlightLen];
    Details      details;
    std::int64_t used_us;
    bool         valid;
};

CacheEntry *s_cache = nullptr;

SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_ctrl;

Aircraft    *s_list       = nullptr;
int          s_count      = 0;
Aircraft    *s_scratch    = nullptr;
Snapshot    *s_published  = nullptr;

// What prefetch_visible() asks about, gathered under the lock and looked up
// after it; in PSRAM, as the lists it is taken from are.
struct Want {
    char  hex[kHexLen];
    char  flight[kFlightLen];
    float distance;
};
Want *s_wanted = nullptr;
bool         s_ok         = false;
std::int64_t s_fetched_us = 0;

UpdateHandler  s_on_update  = nullptr;
DetailsHandler s_on_details = nullptr;
PhotoHandler   s_on_photo   = nullptr;

std::uint16_t *s_photo              = nullptr;
int            s_photo_w            = 0;
int            s_photo_h            = 0;
char           s_photo_hex[kHexLen] = {};

char s_want_hex[kHexLen]       = {};
char s_want_flight[kFlightLen] = {};
bool s_want_pending            = false;

TaskHandle_t s_task    = nullptr;
bool         s_active  = false;
bool         s_enabled = true;
bool         s_screen  = true;  // nothing is fetched while the screen is dark

std::int64_t s_home_at_us = 0;
float        s_home_lat   = 0.0f;
float        s_home_lon   = 0.0f;
bool         s_has_home   = false;

int ms_since(std::int64_t began_us)
{
    return static_cast<int>((esp_timer_get_time() - began_us) / units::kUsPerMs);
}

bool is_https(const char *url)
{
    return std::strncmp(url, HTTPS_PREFIX, HTTPS_PREFIX_LEN) == 0;
}

esp_err_t on_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }
    const std::size_t room = BODY_MAX - 1 - s_body_len;
    const std::size_t take = static_cast<std::size_t>(event->data_len) < room
                                 ? static_cast<std::size_t>(event->data_len)
                                 : room;
    if (take > 0) {
        std::memcpy(s_body + s_body_len, event->data, take);
        s_body_len += take;
        s_body[s_body_len] = '\0';
    }
    return ESP_OK;
}

esp_http_client_handle_t open_client(const char *url, const char *agent = AGENT)
{
    esp_http_client_config_t cfg = {};
    cfg.url                      = url;
    cfg.event_handler            = on_event;
    cfg.timeout_ms               = HTTP_TIMEOUT_MS;
    cfg.user_agent               = agent;
    cfg.buffer_size              = HTTP_BUFFER_SIZE;
    cfg.keep_alive_enable        = true;
    // Only where TLS can actually be negotiated: two of these hosts are plain
    // http, and a root store there is internal memory spent on nothing.
    if (is_https(url)) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    return esp_http_client_init(&cfg);
}

int get(esp_http_client_handle_t client, const char *url, const char *what)
{
    if (client == nullptr || esp_http_client_set_url(client, url) != ESP_OK) {
        return 0;
    }

    s_body_len = 0;
    s_body[0]  = '\0';

    const esp_err_t err    = esp_http_client_perform(client);
    const int       status = esp_http_client_get_status_code(client);

    if (err != ESP_OK) {
        esp_http_client_close(client);
        // `what` rather than the url: the feed's url carries the panel's own
        // coordinates, and these lines show on the diagnostics page.
        ESP_LOGW(TAG, "%s unreachable: %s", what, esp_err_to_name(err));
        return 0;
    }
    if (status == HTTP_NOT_FOUND) {
        ESP_LOGD(TAG, "%s not known", what);
    } else if (status != HTTP_OK) {
        ESP_LOGW(TAG, "%s: http %d", what, status);
    }
    return status;
}

CacheEntry *cache_find(const char *hex, const char *flight)
{
    if (s_cache == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < CACHE_SIZE; ++i) {
        CacheEntry &entry = s_cache[i];
        if (entry.valid && std::strcmp(entry.hex, hex) == 0 &&
            std::strcmp(entry.flight, flight) == 0) {
            entry.used_us = esp_timer_get_time();
            return &entry;
        }
    }
    return nullptr;
}

void cache_put(const char *hex, const char *flight, const Details &details)
{
    if (s_cache == nullptr) {
        return;
    }
    CacheEntry *slot = cache_find(hex, flight);
    if (slot == nullptr) {
        slot = &s_cache[0];
        for (int i = 0; i < CACHE_SIZE; ++i) {
            if (!s_cache[i].valid) {
                slot = &s_cache[i];
                break;
            }
            if (s_cache[i].used_us < slot->used_us) {
                slot = &s_cache[i];
            }
        }
    }
    std::snprintf(slot->hex, sizeof(slot->hex), "%s", hex);
    std::snprintf(slot->flight, sizeof(slot->flight), "%s", flight);
    slot->details = details;
    slot->used_us = esp_timer_get_time();
    slot->valid   = true;
}

Trail *s_trails = nullptr;

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

// Each reading adds to where those in it have been. Under s_lock.
void record_trails(const Aircraft *list, int count)
{
    if (s_trails == nullptr) {
        return;
    }
    const std::int64_t now = esp_timer_get_time();
    for (int i = 0; i < count; ++i) {
        const Aircraft &aircraft = list[i];
        if (aircraft.on_ground || aircraft.hex[0] == '\0') {
            continue;
        }
        Trail *trail   = trail_slot(aircraft.hex, now);
        trail->seen_us = now;
        note(*trail, aircraft.lat, aircraft.lon, TRAIL_STEP_KM);
    }
}

// The feeds are asked for gzip, a seventh of the size, and traces come that
// way whatever is asked for.
char               *s_inflated = nullptr;  // BODY_MAX, as the feed's body would be
tinfl_decompressor *s_inflater = nullptr;  // 11 KB: too much for the task's stack

// The inflated length, or 0 when `in` is not a whole gzip stream.
std::size_t gunzip(const std::uint8_t *in, std::size_t length, char *out, std::size_t size)
{
    constexpr std::size_t  HEADER = 10, TRAILER = 8;
    constexpr std::uint8_t FHCRC = 2, FEXTRA = 4, FNAME = 8, FCOMMENT = 16;
    if (length < HEADER + TRAILER || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
        return 0;
    }
    const std::uint8_t flags = in[3];
    std::size_t        at    = HEADER;
    if ((flags & FEXTRA) != 0) {
        at += 2 + (in[at] | (in[at + 1] << 8));
    }
    for (const std::uint8_t text : {FNAME, FCOMMENT}) {
        if ((flags & text) != 0) {
            while (at < length && in[at] != 0) {
                ++at;
            }
            ++at;
        }
    }
    if ((flags & FHCRC) != 0) {
        at += 2;
    }
    if (at + TRAILER >= length) {
        return 0;
    }
    std::size_t in_bytes  = length - at - TRAILER;
    std::size_t out_bytes = size - 1;
    tinfl_init(s_inflater);
    const tinfl_status status =
        tinfl_decompress(s_inflater, in + at, &in_bytes, reinterpret_cast<mz_uint8 *>(out),
                         reinterpret_cast<mz_uint8 *>(out), &out_bytes, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE) {
        return 0;
    }
    out[out_bytes] = '\0';
    return out_bytes;
}

// The body as JSON: inflated into s_inflated when it came compressed. Null
// when it came compressed and does not inflate.
const char *unpacked(std::size_t &length)
{
    constexpr std::uint8_t GZIP_FIRST = 0x1f;
    if (s_body_len == 0 || static_cast<std::uint8_t>(s_body[0]) != GZIP_FIRST) {
        length = s_body_len;
        return s_body;
    }
    length = gunzip(reinterpret_cast<const std::uint8_t *>(s_body), s_body_len, s_inflated, BODY_MAX);
    return length > 0 ? s_inflated : nullptr;
}

// Two feeds of the same readsb data, taken in turn: one alone refused most
// readings at one every five seconds, the panel sharing its address with
// whatever else in the house asks.
struct Feed {
    const char              *name;
    const char              *host;
    const char              *query;  // host, lat, lon, radius in nautical miles
    bool                     keep_open;
    esp_http_client_handle_t client;
};
Feed s_feeds[] = {
    // Measured: holding this one open doubled the share it refused.
    {"adsb.lol", "http://api.adsb.lol", "%s/v2/point/%.4f/%.4f/%d", false, nullptr},
    // https only, so held open rather than shaking hands every reading.
    {"adsb.fi", "https://opendata.adsb.fi", "%s/api/v2/lat/%.4f/lon/%.4f/dist/%d", true, nullptr},
};
constexpr int FEED_COUNT = static_cast<int>(std::size(s_feeds));
int           s_next_feed = 0;

bool s_feed_backoff = false;

int ask_feed(Feed &feed, float lat, float lon)
{
    char url[URL_SIZE];
    std::snprintf(url, sizeof(url), feed.query, feed.host, static_cast<double>(lat), static_cast<double>(lon),
                  static_cast<int>(std::lround(static_cast<float>(RANGE_KM) * NM_PER_KM)));
    char what[24];
    std::snprintf(what, sizeof(what), "feed %s", feed.name);
    const int status = get(feed.client, url, what);
    if (!feed.keep_open) {
        esp_http_client_close(feed.client);
    }
    return status;
}

// The next feed in turn, and the other at once when that one will not answer.
bool fetch(float lat, float lon)
{
    int  status  = 0;
    int  refused = 0;
    for (int tried = 0; tried < FEED_COUNT && status != HTTP_OK; ++tried) {
        Feed &feed  = s_feeds[s_next_feed];
        s_next_feed = (s_next_feed + 1) % FEED_COUNT;
        status      = ask_feed(feed, lat, lon);
        refused += status == HTTP_TOO_MANY_REQUESTS ? 1 : 0;
    }
    s_feed_backoff = refused == FEED_COUNT;
    std::size_t json_len = 0;
    const char *json     = status == HTTP_OK ? unpacked(json_len) : nullptr;
    if (json == nullptr) {
        return false;
    }

    const int count = parse(json, json_len, s_scratch, kMaxAircraft);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::memcpy(s_list, s_scratch, sizeof(Aircraft) * static_cast<std::size_t>(count));
    record_trails(s_list, count);
    s_count      = count;
    s_ok         = true;
    s_fetched_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);

    ESP_LOGD(TAG, "%d aircraft within %d km, %u bytes", count, RANGE_KM,
             static_cast<unsigned>(json_len));

    if (s_on_update != nullptr) {
        snapshot(*s_published);
        s_on_update(*s_published);
    }
    return true;
}

void fetch_photo(const char *hex, const Details &details)
{
    if (s_photo != nullptr && s_photo_w > 0 && std::strcmp(s_photo_hex, hex) == 0) {
        if (s_on_photo != nullptr) {
            s_on_photo(hex, s_photo, s_photo_w, s_photo_h);
        }
        return;
    }

    if (s_photo == nullptr || details.photo_url[0] == '\0') {
        if (s_on_photo != nullptr) {
            s_on_photo(hex, nullptr, 0, 0);
        }
        return;
    }

    const std::int64_t began  = esp_timer_get_time();
    int                width  = 0;
    int                height = 0;
    const int status = get(s_photo_client, details.photo_url, "photo");
    s_photo_at_us    = esp_timer_get_time();
    s_photo_open     = true;
    if (status == HTTP_OK &&
        jpeg::decode_into(s_body, s_body_len, s_photo, PHOTO_MAX_W, PHOTO_MAX_H, width, height)) {
        s_photo_w = width;
        s_photo_h = height;
        std::snprintf(s_photo_hex, sizeof(s_photo_hex), "%s", hex);
        ESP_LOGI(TAG, "%s: photo %dx%d in %d ms", hex, width, height, ms_since(began));
        if (s_on_photo != nullptr) {
            s_on_photo(hex, s_photo, width, height);
        }
        return;
    }
    s_photo_hex[0] = '\0';
    s_photo_w      = 0;
    if (s_on_photo != nullptr) {
        s_on_photo(hex, nullptr, 0, 0);
    }
}

bool fetch_details(const char *hex, const char *callsign, Details &out)
{
    char url[LOOKUP_URL_SIZE];
    bool want_aircraft = true;
    bool want_route    = callsign[0] != '\0';

    // One request answers for the airframe and the flight it is on, which is one
    // handshake instead of two -- but it is all or nothing: an unknown callsign
    // 404s the whole thing. The body says which half was missing, so only that
    // half is asked for again.
    if (want_route) {
        std::snprintf(url, sizeof(url), "%s/v0/aircraft/%s?callsign=%s", LOOKUP_HOST, hex,
                      callsign);
        const int status = get(s_lookup_client, url, "details");
        if (status == HTTP_OK) {
            parse_aircraft(s_body, s_body_len, out);
            parse_route(s_body, s_body_len, out);
            return true;
        }
        if (status != HTTP_NOT_FOUND) {
            return false;
        }
        want_route    = std::strstr(s_body, "unknown callsign") == nullptr;
        want_aircraft = std::strstr(s_body, "unknown aircraft") == nullptr;
    }

    if (want_aircraft) {
        std::snprintf(url, sizeof(url), "%s/v0/aircraft/%s", LOOKUP_HOST, hex);
        if (get(s_lookup_client, url, "aircraft") == HTTP_OK) {
            parse_aircraft(s_body, s_body_len, out);
        }
    }
    if (want_route) {
        std::snprintf(url, sizeof(url), "%s/v0/callsign/%s", LOOKUP_HOST, callsign);
        if (get(s_lookup_client, url, "route") == HTTP_OK) {
            parse_route(s_body, s_body_len, out);
        }
    }
    return out.has_aircraft || out.has_route;
}

void close_idle_lookups()
{
    const std::int64_t now = esp_timer_get_time();
    if (s_photo_open && now - s_photo_at_us >= PHOTO_IDLE_US) {
        esp_http_client_close(s_photo_client);
        s_photo_open = false;
    }
    if (s_lookups_open && now - s_lookup_at_us >= IDLE_CLOSE_US) {
        esp_http_client_close(s_lookup_client);
        esp_http_client_close(s_photoapi_client);
        esp_http_client_close(s_trace_client);
        s_lookups_open = false;
    }
}

void resolve_photo(const char *hex, Details &out)
{
    out.photo_checked = true;

    char url[URL_SIZE];
    std::snprintf(url, sizeof(url), "%s/pub/photos/hex/%s", PHOTO_HOST, hex);
    if (get(s_photoapi_client, url, "photo lookup") != HTTP_OK) {
        return;
    }

    char found[sizeof(out.photo_url)];
    if (!parse_photo(s_body, s_body_len, found, sizeof(found))) {
        return;
    }
    if (is_https(found)) {
        std::snprintf(out.photo_url, sizeof(out.photo_url), "http://%s",
                      found + HTTPS_PREFIX_LEN);
    } else {
        std::snprintf(out.photo_url, sizeof(out.photo_url), "%s", found);
    }
}

// Where a tapped aircraft has been this last quarter hour, before the panel
// was watching, so its trail reaches back across the view at once.
void fetch_trace(const char *hex)
{
    const std::size_t len = std::strlen(hex);
    if (s_inflated == nullptr || s_inflater == nullptr || len < 2 || hex[0] == '~') {
        return;  // a ~ is a position from ground radar, which has no trace
    }
    char url[URL_SIZE];
    std::snprintf(url, sizeof(url), "%s/data/traces/%.2s/trace_recent_%.7s.json", TRACE_HOST, hex + len - 2, hex);
    if (get(s_trace_client, url, "trace") != HTTP_OK) {
        return;
    }
    std::size_t  json_len = 0;
    const char  *json     = unpacked(json_len);
    static Trail trace;  // only this task
    trace = Trail{};
    if (json == nullptr || parse_trace(json, json_len, trace, TRAIL_STEP_KM) <= 0) {
        ESP_LOGW(TAG, "%s: no trace in %u bytes", hex, static_cast<unsigned>(s_body_len));
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const std::int64_t now  = esp_timer_get_time();
    Trail             *slot = trail_slot(hex, now);
    std::memcpy(trace.hex, slot->hex, sizeof(trace.hex));
    trace.seen_us = now;
    *slot         = trace;
    xSemaphoreGive(s_lock);

    if (s_on_update != nullptr) {
        snapshot(*s_published);
        s_on_update(*s_published);
    }
}

void look_up(const char *hex, const char *callsign, bool with_photo)
{
    Details           details{};
    CacheEntry *hit = cache_find(hex, callsign);

    s_lookup_at_us = esp_timer_get_time();
    s_lookups_open = true;

    if (hit != nullptr) {
        details = hit->details;
    } else {
        const std::int64_t began = esp_timer_get_time();
        fetch_details(hex, callsign, details);
        cache_put(hex, callsign, details);
        hit = cache_find(hex, callsign);
        ESP_LOGI(TAG, "%s: %s%s%s in %d ms", hex,
                 details.has_aircraft ? details.model : "unknown type",
                 details.has_route ? ", " : "", details.has_route ? details.origin_code : "",
                 ms_since(began));
    }

    if (with_photo && !details.photo_checked) {
        resolve_photo(hex, details);
        if (hit != nullptr) {
            hit->details = details;
        }
    }

    if (s_on_details != nullptr) {
        s_on_details(hex, details);
    }
    if (with_photo) {
        fetch_trace(hex);
        fetch_photo(hex, details);
    }
}

void warm_photo(const char *hex, const char *callsign)
{
    CacheEntry *entry = cache_find(hex, callsign);
    if (entry == nullptr || entry->details.photo_checked) {
        return;
    }
    resolve_photo(hex, entry->details);
}

bool tap_waiting()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool waiting = s_want_pending;
    xSemaphoreGive(s_lock);
    return waiting;
}

void prefetch_visible()
{
    Want *wanted = s_wanted;
    int   count  = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && count < kMaxAircraft; ++i) {
        const Aircraft &aircraft = s_list[i];
        if (aircraft.on_ground || aircraft.hex[0] == '\0') {
            continue;
        }
        std::memcpy(wanted[count].hex, aircraft.hex, sizeof(wanted[count].hex));
        std::memcpy(wanted[count].flight, aircraft.flight, sizeof(wanted[count].flight));
        wanted[count].distance = aircraft.distance_nm;
        ++count;
    }
    xSemaphoreGive(s_lock);

    std::sort(wanted, wanted + count,
              [](const Want &a, const Want &b) { return a.distance < b.distance; });

    int chosen[PREFETCH_PER_SWEEP];
    int fetched = 0;
    for (int i = 0; i < count && i < PREFETCH_NEAREST && fetched < PREFETCH_PER_SWEEP; ++i) {
        if (tap_waiting()) {
            break;
        }
        if (cache_find(wanted[i].hex, wanted[i].flight) != nullptr) {
            continue;
        }
        look_up(wanted[i].hex, wanted[i].flight, false);
        chosen[fetched++] = i;
    }
    if (fetched == 0) {
        return;
    }

    esp_http_client_close(s_lookup_client);
    for (int i = 0; i < fetched; ++i) {
        if (tap_waiting()) {
            break;
        }
        warm_photo(wanted[chosen[i]].hex, wanted[chosen[i]].flight);
    }
    esp_http_client_close(s_photoapi_client);
}

void expire_cache()
{
    if (s_cache == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (!s_cache[i].valid) {
            continue;
        }
        bool still_here = false;
        for (int j = 0; j < s_count && !still_here; ++j) {
            still_here = std::strcmp(s_cache[i].hex, s_list[j].hex) == 0;
        }
        s_cache[i].valid = still_here;
    }
    xSemaphoreGive(s_lock);
}

// Call with the lock held.
int age_s()
{
    return s_fetched_us == 0 ? -1
                             : static_cast<int>((esp_timer_get_time() - s_fetched_us) /
                                                units::kUsPerSecond);
}

void sweep(float lat, float lon, bool active)
{
    if (fetch(lat, lon)) {
        expire_cache();
        if (active) {
            prefetch_visible();
        }
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ok = false;
    xSemaphoreGive(s_lock);
}

TickType_t rest_before_next(std::int64_t due_us, std::int64_t last_fetch_us)
{
    const std::int64_t waited = esp_timer_get_time() - last_fetch_us;
    std::int64_t rest_ms = waited >= due_us ? OVERDUE_REST_MS : (due_us - waited) / units::kUsPerMs;
    if ((s_photo_open || s_lookups_open) && rest_ms > LOOKUPS_OPEN_REST_MS) {
        rest_ms = LOOKUPS_OPEN_REST_MS;
    }
    return pdMS_TO_TICKS(rest_ms);
}

[[noreturn]] void radar_task(void *)
{
    std::int64_t last_fetch = 0;

    for (;;) {
        char hex[kHexLen]         = {};
        char callsign[kFlightLen] = {};

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool         ready   = s_has_home && s_enabled && s_screen;
        const bool         active  = s_active;
        const bool         pending = s_want_pending;
        const float        lat     = s_home_lat;
        const float        lon     = s_home_lon;
        const std::int64_t home_at = s_home_at_us;
        std::memcpy(hex, s_want_hex, sizeof(hex));
        std::memcpy(callsign, s_want_flight, sizeof(callsign));
        s_want_pending = false;
        xSemaphoreGive(s_lock);

        if (pending) {
            look_up(hex, callsign, true);
            continue;
        }

        if (!ready) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (last_fetch == 0 && esp_timer_get_time() - home_at < FIRST_FETCH_DELAY_US) {
            ulTaskNotifyTake(pdTRUE, HOME_SETTLE_CHECK);
            continue;
        }

        std::int64_t due = active ? POLL_ACTIVE_US : POLL_IDLE_US;
        if (s_feed_backoff) {
            due *= 2;
        }
        if (last_fetch == 0 || esp_timer_get_time() - last_fetch >= due) {
            const std::int64_t began = esp_timer_get_time();
            sweep(lat, lon, active);
            last_fetch = began;
        }

        close_idle_lookups();
        ulTaskNotifyTake(pdTRUE, rest_before_next(due, last_fetch));
    }
}

}  // namespace

esp_err_t start(UpdateHandler on_update, DetailsHandler on_details, PhotoHandler on_photo)
{
    s_on_update  = on_update;
    s_on_details = on_details;
    s_on_photo   = on_photo;

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, TAG, "lock");

    s_body = static_cast<char *>(heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_body != nullptr, ESP_ERR_NO_MEM, TAG, "body buffer");

    s_photo = static_cast<std::uint16_t *>(heap_caps_malloc(
        static_cast<std::size_t>(PHOTO_MAX_W) * PHOTO_MAX_H * sizeof(std::uint16_t),
        MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(s_photo != nullptr, ESP_ERR_NO_MEM, TAG, "photo buffer");

    s_list      = static_cast<Aircraft *>(
        heap_caps_calloc(kMaxAircraft, sizeof(Aircraft), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_scratch   = static_cast<Aircraft *>(
        heap_caps_calloc(kMaxAircraft, sizeof(Aircraft), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_published = static_cast<Snapshot *>(
        heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_wanted = static_cast<Want *>(
        heap_caps_calloc(kMaxAircraft, sizeof(Want), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_list != nullptr && s_scratch != nullptr && s_published != nullptr &&
                            s_wanted != nullptr,
                        ESP_ERR_NO_MEM, TAG, "aircraft buffers");

    s_trails = static_cast<Trail *>(
        heap_caps_calloc(TRAIL_SLOTS, sizeof(Trail), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_trails != nullptr, ESP_ERR_NO_MEM, TAG, "trails");
    s_inflated = static_cast<char *>(heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_inflater = static_cast<tinfl_decompressor *>(
        heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_inflated != nullptr && s_inflater != nullptr, ESP_ERR_NO_MEM, TAG, "inflate buffers");

    s_cache = static_cast<CacheEntry *>(
        heap_caps_calloc(CACHE_SIZE, sizeof(CacheEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_cache != nullptr, ESP_ERR_NO_MEM, TAG, "lookup cache");

    for (Feed &feed : s_feeds) {
        feed.client = open_client(feed.host);
        ESP_RETURN_ON_FALSE(feed.client != nullptr, ESP_ERR_NO_MEM, TAG, "feed client");
        esp_http_client_set_header(feed.client, "Accept-Encoding", "gzip");
    }
    s_lookup_client   = open_client(LOOKUP_HOST);
    s_photoapi_client = open_client(PHOTO_HOST, PHOTO_AGENT);
    s_photo_client    = open_client(PHOTO_IMAGE_HOST);
    s_trace_client    = open_client(TRACE_HOST);
    ESP_RETURN_ON_FALSE(s_lookup_client != nullptr &&
                            s_photoapi_client != nullptr && s_photo_client != nullptr && s_trace_client != nullptr,
                        ESP_ERR_NO_MEM, TAG, "http clients");

    s_task = xTaskCreateStaticPinnedToCore(radar_task, "radar", TASK_STACK, nullptr,
                                           TASK_PRIORITY, s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void set_active(bool active)
{
    if (s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool woke = active && !s_active;
    s_active        = active;
    xSemaphoreGive(s_lock);

    if (woke && s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void request_details(const char *hex, const char *callsign)
{
    if (s_lock == nullptr || hex == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::snprintf(s_want_hex, sizeof(s_want_hex), "%s", hex);
    std::snprintf(s_want_flight, sizeof(s_want_flight), "%s", callsign != nullptr ? callsign : "");
    s_want_pending = true;
    xSemaphoreGive(s_lock);

    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void set_screen(bool on)
{
    if (s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool woke = on && !s_screen;
    s_screen        = on;
    xSemaphoreGive(s_lock);

    if (woke && s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void set_enabled(bool enabled)
{
    if (s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool woke = enabled && !s_enabled;
    s_enabled       = enabled;
    xSemaphoreGive(s_lock);

    if (woke && s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void set_home(float lat, float lon)
{
    if (s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool first = !s_has_home;
    if (first) {
        s_home_at_us = esp_timer_get_time();
    }
    s_home_lat       = std::round(lat / HOME_GRID_DEG) * HOME_GRID_DEG;
    s_home_lon       = std::round(lon / HOME_GRID_DEG) * HOME_GRID_DEG;
    s_has_home       = true;
    xSemaphoreGive(s_lock);

    if (first) {
        ESP_LOGI(TAG, "centred on Home Assistant's home zone");
        if (s_task != nullptr) {
            xTaskNotifyGive(s_task);
        }
    }
}

int trail(const char *hex, TrailPoint *out, int max)
{
    if (s_lock == nullptr || s_trails == nullptr || hex == nullptr || hex[0] == '\0') {
        return 0;
    }
    int count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < TRAIL_SLOTS; ++i) {
        if (std::strcmp(s_trails[i].hex, hex) == 0) {
            count = oldest_first(s_trails[i], out, max);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return count;
}

void snapshot(Snapshot &out)
{
    if (s_lock == nullptr) {
        out = Snapshot{};
        out.age_s = -1;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::memcpy(out.list, s_list, sizeof(Aircraft) * static_cast<std::size_t>(s_count));
    out.count    = s_count;
    out.home_lat = s_home_lat;
    out.home_lon = s_home_lon;
    out.range_km = RANGE_KM;
    out.ok       = s_ok;
    out.age_s    = age_s();
    xSemaphoreGive(s_lock);
}

void status(Status &out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out.count    = s_count;
    out.range_km = RANGE_KM;
    out.ok       = s_ok;
    out.age_s    = age_s();
    xSemaphoreGive(s_lock);
}

}  // namespace radar
