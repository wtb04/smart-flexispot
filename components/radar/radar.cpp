#include "radar.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "media.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace radar {
namespace {

constexpr char TAG[] = "radar";

// adsb.lol's open feed: no account, no key, and it returns the distance and
// bearing from the point asked about, which saves doing it here. Taken over
// plain http, which it serves without redirecting, so the sweep no longer sets
// up a TLS session every few seconds -- that session is drawn from the same
// internal memory the Bluetooth and AES drivers need, and it was the largest
// recurring claim on it. Compared against adsb.fi at the same moment: the same
// twenty-one aircraft, to the hex. Its data is ODbL.
constexpr char FEED_HOST[]  = "http://api.adsb.lol";
// Who is flying it and where it is going. Also keyless, and asked only about
// whatever aircraft has been tapped or is about to be.
constexpr char LOOKUP_HOST[] = "https://api.adsbdb.com";

// adsbdb has a picture for about a third of what flies past here; planespotters
// has one for four fifths, and covers everything adsbdb does. Their thumbnails
// also come off a plain http host, so the one request per photograph that used
// to need a TLS session now needs none at all. The api itself refuses a request
// whose agent carries no way of getting in touch.
constexpr char PHOTO_HOST[]  = "https://api.planespotters.net";
constexpr char PHOTO_AGENT[] = "tab5-panel (+https://woutertenbrinke.nl)";
constexpr char AGENT[]       = "tab5-panel";

// Asked for in nautical miles because that is what the feed takes, shown in
// kilometres because that is what the rings are labelled in. Four rings across
// eighty land on 20, 40, 60 and 80.
// How far the scope reaches, in kilometres, chosen from the page. The feed
// takes nautical miles.
// Always the farthest the page can be set to. One sweep covers every zoom
// level, so changing the zoom shows the right aircraft at once instead of the
// previous range's until the next request comes back.
constexpr int    RANGE_DEFAULT_KM = 160;

// How coarsely the home position is rounded before anything is asked about it.
constexpr float HOME_GRID_DEG = 0.01f;
constexpr float  NM_PER_KM        = 0.539957f;
std::atomic<int>  s_range_km{RANGE_DEFAULT_KM};
std::atomic<bool> s_force_fetch{false};

// Big enough for the thumbnails adsbdb points at, which run to a few hundred
// pixels across.
constexpr int PHOTO_MAX_W = 320;
constexpr int PHOTO_MAX_H = 240;

// The feed asks for no more than one request a second; the fast rate is a fifth
// of that, and costs nothing but bytes now that it needs no handshake.
// The slow one only has to keep the page from opening on nothing.
// The home position lands while the broker, the socket and Bluetooth are all
// still coming up, and a TLS handshake thrown in on top of that has failed for
// want of internal memory. A few seconds is enough for the rest to settle.
constexpr std::int64_t FIRST_FETCH_DELAY_US = 6 * 1000000LL;

constexpr std::int64_t POLL_ACTIVE_US = 5 * 1000000LL;
constexpr std::int64_t POLL_IDLE_US   = 60 * 1000000LL;

// Forty nautical miles over a busy corner of Europe is about 25 kB; this is
// room for a far denser sky. It lives in PSRAM, where it is not missed.
constexpr std::size_t BODY_MAX = 96 * 1024;

// An https handshake and the mbedtls session live on this stack, and the
// certificate bundle is walked on it too.
constexpr std::uint32_t TASK_STACK    = 8192;  // measured: uses 3.1 KB; the TLS handshake runs on it
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

char        *s_body     = nullptr;
std::size_t  s_body_len = 0;

// One client per host, held open. Measured against adsbdb from a desktop, the
// TLS handshake was 36 ms of a 51 ms request and a second request down the same
// connection took 14 ms; on this part a handshake is far more than 70% of the
// cost, so reusing the connection is most of what makes a lookup quick.
// An open TLS session costs internal memory that only DMA can use, and this
// part has little of it: holding all three open cost 47 KB of the 72 KB free
// and the AES driver started failing to get descriptors. So a session is kept
// only while it is buying something. Lookups and their pictures come in bursts
// -- a tap, or the sweep's prefetch -- so those connections are kept between
// requests and given back once nobody has asked for anything in a while. The
// feed is polled on nobody's critical path, so its handshake is free.
//
// Measured: the picture host is a slow origin, and reconnecting to it for every
// photograph cost between 1.6 and 2.8 seconds against 120 ms down a connection
// already open. That is worth the 9 KB it holds while the page is in use.
constexpr std::int64_t IDLE_CLOSE_US = 20 * 1000000LL;

// The picture host gets a far shorter leash than the lookup host. Tapping
// around the scope fetches several photographs in a row, so the connection is
// worth keeping through a burst, but between bursts it is holding memory the
// TLS and AES drivers need and nothing is using it. Twenty seconds of that was
// enough to run the DMA-capable heap down to thirteen kilobytes.
constexpr std::int64_t PHOTO_IDLE_US = 5 * 1000000LL;

esp_http_client_handle_t s_feed_client     = nullptr;
esp_http_client_handle_t s_lookup_client   = nullptr;
esp_http_client_handle_t s_photoapi_client = nullptr;
esp_http_client_handle_t s_photo_client    = nullptr;

std::int64_t s_lookup_at_us = 0;
bool         s_lookups_open = false;
std::int64_t s_photo_at_us  = 0;
bool         s_photo_open   = false;

// There are rarely more than a dozen aircraft in range, and what is known about
// one never changes, so the whole scope can be warmed rather than just the
// aircraft most likely to be tapped. Spread over a few sweeps so a busy sky
// does not turn into a burst of requests at a free database.
constexpr int PREFETCH_PER_SWEEP = 3;

// Cleared on the radar task rather than wherever the page was navigated away
// from, since the cache belongs to that task.
std::atomic<bool> s_drop_cache{false};

// Nothing looked up ever changes, so nothing needs looking up twice: a route
// belongs to a callsign and a registration to an airframe. Keyed on both,
// because a hex flies under a different callsign tomorrow. Failures are kept
// too -- an aircraft the database has never heard of will not have appeared in
// it by the next tap.
constexpr int CACHE_SIZE = 24;

struct CacheEntry {
    char         hex[kHexLen];
    char         flight[kFlightLen];
    Details      details;
    std::int64_t used_us;
    bool         valid;
};

// A few kilobytes, and nothing here is wanted in a hurry from an interrupt.
CacheEntry *s_cache = nullptr;

SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_ctrl;

// In PSRAM, not in .bss. An aircraft record is 108 bytes and there are four
// copies of the list between the parser and the screen, which at sixty aircraft
// is thirty kilobytes of the only internal pool DMA can reach -- the pool the
// Wi-Fi transport asserts without. Nothing here is touched from an interrupt or
// with the cache off, so none of it has any business being there.
Aircraft    *s_list = nullptr;
int          s_count      = 0;

// Static rather than automatic: an Aircraft is sixty-odd bytes and forty of
// them is two and a half kilobytes, which is not something to put on a stack
// that is already carrying a TLS session.
Aircraft *s_scratch  = nullptr;
Snapshot *s_published = nullptr;
bool         s_ok         = false;
std::int64_t s_fetched_us = 0;

UpdateHandler  s_on_update  = nullptr;
DetailsHandler s_on_details = nullptr;
PhotoHandler   s_on_photo   = nullptr;

std::uint16_t *s_photo    = nullptr;
int            s_photo_w  = 0;
int            s_photo_h  = 0;
char           s_photo_hex[kHexLen] = {};

char    s_want_hex[kHexLen]      = {};
char    s_want_flight[kFlightLen] = {};
bool    s_want_pending           = false;
Details s_details;
TaskHandle_t  s_task      = nullptr;
bool          s_active    = false;
bool          s_enabled   = true;

std::int64_t s_home_at_us = 0;

float s_home_lat  = 0.0f;
float s_home_lon  = 0.0f;
bool  s_has_home  = false;

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

// The certificate bundle is attached only where it can be needed. Two of these
// hosts are spoken to over plain http, and a client that can never negotiate
// TLS has no use for a root store.
esp_http_client_handle_t open_client(const char *url, const char *agent = AGENT)
{
    esp_http_client_config_t cfg = {};
    cfg.url                      = url;
    cfg.event_handler            = on_event;
    cfg.timeout_ms               = 10000;
    cfg.user_agent               = agent;
    cfg.buffer_size              = 2048;
    cfg.keep_alive_enable        = true;
    if (std::strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    return esp_http_client_init(&cfg);
}

// `what` rather than the url: the feed's url has the panel's own coordinates
// in its path, and these lines are readable on the diagnostics page.
//
// Returns the http status, or zero if the request never got that far. 404 is an
// ordinary answer here -- a callsign the database has never heard of -- and the
// body says which half of a combined lookup was missing, so the caller wants to
// tell it apart from a failure.
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
        // Retrying down a connection that has been closed at the other end
        // fails the same way, so it goes rather than being kept for next time.
        esp_http_client_close(client);
        ESP_LOGW(TAG, "%s unreachable: %s", what, esp_err_to_name(err));
        return 0;
    }
    if (status == 404) {
        ESP_LOGD(TAG, "%s not known", what);
    } else if (status != 200) {
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

// Set when the feed says it has had enough, and honoured by skipping a sweep.
// Free to use and asking for no more than a request a second, so being told to
// slow down should actually slow something down.
bool s_feed_backoff = false;

bool fetch(float lat, float lon)
{
    const int range_km = s_range_km.load(std::memory_order_relaxed);

    char url[128];
    std::snprintf(url, sizeof(url), "%s/v2/point/%.4f/%.4f/%d", FEED_HOST,
                  static_cast<double>(lat), static_cast<double>(lon),
                  static_cast<int>(std::lround(static_cast<float>(range_km) * NM_PER_KM)));
    const int status = get(s_feed_client, url, "feed");
    esp_http_client_close(s_feed_client);
    if (status != 200) {
        s_feed_backoff = status == 429;
        return false;
    }
    s_feed_backoff = false;

    const int count = parse(s_body, s_body_len, s_scratch, kMaxAircraft);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::memcpy(s_list, s_scratch, sizeof(Aircraft) * static_cast<std::size_t>(count));
    s_count      = count;
    s_ok         = true;
    s_fetched_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "%d aircraft within %d km, %u bytes", count, range_km,
             static_cast<unsigned>(s_body_len));

    if (s_on_update != nullptr) {
        snapshot(*s_published);
        s_on_update(*s_published);
    }
    return true;
}

void fetch_photo(const char *hex, const Details &details)
{
    // Already decoded and still in the buffer: selecting the same aircraft
    // again should not cost a download, and the nearest one is selected over
    // and over as the page refreshes.
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
    if (status == 200 &&
        media::decode_image(s_body, s_body_len, s_photo, PHOTO_MAX_W, PHOTO_MAX_H, width,
                            height)) {
        s_photo_w = width;
        s_photo_h = height;
        std::snprintf(s_photo_hex, sizeof(s_photo_hex), "%s", hex);
        ESP_LOGI(TAG, "%s: photo %dx%d in %d ms", hex, width, height,
                 static_cast<int>((esp_timer_get_time() - began) / 1000));
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

// adsbdb answers for the airframe and the flight it is on in a single request,
// which is the difference between one handshake and two. It is all or nothing,
// though: an unknown callsign 404s the whole thing and takes the aircraft down
// with it. The body says which half was missing, so only that half is asked
// for again and a lookup never costs more than the two it used to.
bool fetch_details(const char *hex, const char *callsign, Details &out)
{
    char url[192];
    bool want_aircraft = true;
    bool want_route    = callsign[0] != '\0';

    if (want_route) {
        std::snprintf(url, sizeof(url), "%s/v0/aircraft/%s?callsign=%s", LOOKUP_HOST, hex,
                      callsign);
        const int status = get(s_lookup_client, url, "details");
        if (status == 200) {
            parse_aircraft(s_body, s_body_len, out);
            parse_route(s_body, s_body_len, out);
            return true;
        }
        if (status != 404) {
            return false;
        }
        want_route    = std::strstr(s_body, "unknown callsign") == nullptr;
        want_aircraft = std::strstr(s_body, "unknown aircraft") == nullptr;
    }

    if (want_aircraft) {
        std::snprintf(url, sizeof(url), "%s/v0/aircraft/%s", LOOKUP_HOST, hex);
        if (get(s_lookup_client, url, "aircraft") == 200) {
            parse_aircraft(s_body, s_body_len, out);
        }
    }
    if (want_route) {
        std::snprintf(url, sizeof(url), "%s/v0/callsign/%s", LOOKUP_HOST, callsign);
        if (get(s_lookup_client, url, "route") == 200) {
            parse_route(s_body, s_body_len, out);
        }
    }
    return out.has_aircraft || out.has_route;
}

// Given back once nobody has looked anything up for a while. Reconnecting costs
// a handshake, but only after the page has been sitting idle, which is not a
// moment anybody is waiting on.
void close_idle_lookups()
{
    const std::int64_t now = esp_timer_get_time();
    if (s_photo_open && now - s_photo_at_us >= PHOTO_IDLE_US) {
        esp_http_client_close(s_photo_client);
        s_photo_open = false;
    }
    if (s_lookups_open && now - s_lookup_at_us >= IDLE_CLOSE_US) {
        esp_http_client_close(s_lookup_client);
        // Warmed on the same burst as the lookups, so it is given back with
        // them; leaving it out held a TLS session open for the whole uptime.
        esp_http_client_close(s_photoapi_client);
        s_lookups_open = false;
    }
}

// `with_photo` is false when warming the cache: only the selected aircraft's
// picture is ever shown, so fetching one per aircraft downloaded and decoded
// a dozen images nobody would see, and held the connection to the picture
// host open for the whole time the page was up. That connection costs
// DMA-capable memory, of which this part has very little.
// Asked once per aircraft and remembered with the rest of what is known about
// it, including the answer "no photograph", so the burst of warming requests at
// the start of a sweep is the only time this host is talked to. Downgraded to
// plain http deliberately: the picture is public, the thumbnails are served
// over it without a redirect, and a TLS session costs memory the AES and
// Bluetooth drivers are already short of.
void resolve_photo(const char *hex, Details &out)
{
    out.photo_checked = true;

    char url[128];
    std::snprintf(url, sizeof(url), "%s/pub/photos/hex/%s", PHOTO_HOST, hex);
    if (get(s_photoapi_client, url, "photo lookup") != 200) {
        return;
    }

    char found[sizeof(out.photo_url)];
    if (!parse_photo(s_body, s_body_len, found, sizeof(found))) {
        // Nothing on file there; whatever adsbdb offered is still worth a try.
        return;
    }
    if (std::strncmp(found, "https://", 8) == 0) {
        std::snprintf(out.photo_url, sizeof(out.photo_url), "http://%s", found + 8);
    } else {
        std::snprintf(out.photo_url, sizeof(out.photo_url), "%s", found);
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
        // Timed because how long a lookup takes is the whole question about it,
        // and the answer depends on whether the connection was still open.
        const std::int64_t began = esp_timer_get_time();
        fetch_details(hex, callsign, details);
        cache_put(hex, callsign, details);
        hit = cache_find(hex, callsign);
        ESP_LOGI(TAG, "%s: %s%s%s in %d ms", hex,
                 details.has_aircraft ? details.model : "unknown type",
                 details.has_route ? ", " : "", details.has_route ? details.origin_code : "",
                 static_cast<int>((esp_timer_get_time() - began) / 1000));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_details = details;
    xSemaphoreGive(s_lock);

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
        fetch_photo(hex, details);
    }
}

// Asked for separately from the details, because the two answers come from
// different hosts: doing them aircraft by aircraft left a TLS session open to
// both at once, and each of those is drawn from the same internal memory the
// Bluetooth and AES drivers are short of.
void warm_photo(const char *hex, const char *callsign)
{
    CacheEntry *entry = cache_find(hex, callsign);
    if (entry == nullptr || entry->details.photo_checked) {
        return;
    }
    resolve_photo(hex, entry->details);
}

// Everything in range gets looked up before anybody asks for it: the only
// lookup that feels instant is one that has already happened, and a scope this
// empty can hold all of it. Nearest first, so a tap lands on a warm entry even
// while the far side of the scope is still filling.
void prefetch_visible()
{
    struct Want {
        char  hex[kHexLen];
        char  flight[kFlightLen];
        float distance;
    };
    static Want wanted[kMaxAircraft];
    int         count = 0;

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

    // Details for all of them first, then photographs for all of them, so only
    // one of the two hosts is ever connected at a time.
    int chosen[PREFETCH_PER_SWEEP];
    int fetched = 0;
    for (int i = 0; i < count && fetched < PREFETCH_PER_SWEEP; ++i) {
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
        warm_photo(wanted[chosen[i]].hex, wanted[chosen[i]].flight);
    }
    esp_http_client_close(s_photoapi_client);
}

// An aircraft that has left the scope will not be tapped, so what was learned
// about it is no longer worth the room. Everything goes when the page does.
void expire_cache()
{
    if (s_cache == nullptr) {
        return;
    }
    if (s_drop_cache.exchange(false, std::memory_order_relaxed)) {
        for (int i = 0; i < CACHE_SIZE; ++i) {
            s_cache[i].valid = false;
        }
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

[[noreturn]] void radar_task(void *)
{
    std::int64_t last_fetch = 0;

    for (;;) {
        char hex[kHexLen]       = {};
        char callsign[kFlightLen] = {};

        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool  ready   = s_has_home && s_enabled;
        const bool  active  = s_active;
        const bool  pending = s_want_pending;
        const float        lat     = s_home_lat;
        const float        lon     = s_home_lon;
        const std::int64_t home_at = s_home_at_us;
        std::memcpy(hex, s_want_hex, sizeof(hex));
        std::memcpy(callsign, s_want_flight, sizeof(callsign));
        s_want_pending = false;
        xSemaphoreGive(s_lock);

        if (s_drop_cache.load(std::memory_order_relaxed)) {
            expire_cache();
        }

        // A tap is waiting on an answer, so it goes before the next sweep.
        if (pending) {
            look_up(hex, callsign, true);
            continue;
        }

        // Nothing to do until there is a position to centre on and a page that
        // can be reached. Both of those arrive with a notification, so there is
        // no reason to wake up and look.
        if (!ready) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (last_fetch == 0 && esp_timer_get_time() - home_at < FIRST_FETCH_DELAY_US) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }

        if (s_force_fetch.exchange(false, std::memory_order_relaxed)) {
            last_fetch = 0;
        }

        std::int64_t due = active ? POLL_ACTIVE_US : POLL_IDLE_US;
        // Doubled rather than more: the refusals come in ones rather than in
        // runs, and three skipped sweeps to answer a single one is a visible
        // gap on a page somebody is watching.
        if (s_feed_backoff) {
            due *= 2;
        }
        if (last_fetch == 0 || esp_timer_get_time() - last_fetch >= due) {
            if (fetch(lat, lon)) {
                expire_cache();
                if (active) {
                    prefetch_visible();
                }
            } else {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_ok = false;
                xSemaphoreGive(s_lock);
            }
            last_fetch = esp_timer_get_time();
        }

        close_idle_lookups();

        // A notification cuts the wait short, which is how opening the page
        // gets a reading straight away rather than up to a minute later.
        const std::int64_t waited = esp_timer_get_time() - last_fetch;
        std::int64_t       rest   = waited >= due ? 1000 : (due - waited) / 1000;
        // A connection waiting to be given back is not worth a ten second wait.
        if ((s_photo_open || s_lookups_open) && rest > 2000) {
            rest = 2000;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(rest));
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
        static_cast<std::size_t>(PHOTO_MAX_W) * PHOTO_MAX_H * 2, MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(s_photo != nullptr, ESP_ERR_NO_MEM, TAG, "photo buffer");

    s_list      = static_cast<Aircraft *>(
        heap_caps_calloc(kMaxAircraft, sizeof(Aircraft), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_scratch   = static_cast<Aircraft *>(
        heap_caps_calloc(kMaxAircraft, sizeof(Aircraft), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_published = static_cast<Snapshot *>(
        heap_caps_calloc(1, sizeof(Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_list != nullptr && s_scratch != nullptr && s_published != nullptr,
                        ESP_ERR_NO_MEM, TAG, "aircraft buffers");

    s_cache = static_cast<CacheEntry *>(
        heap_caps_calloc(CACHE_SIZE, sizeof(CacheEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_cache != nullptr, ESP_ERR_NO_MEM, TAG, "lookup cache");

    // Opened against each host up front so the url is all that changes later:
    // a client given a new url on the same host keeps the connection, which is
    // the whole point of holding on to them.
    s_feed_client     = open_client(FEED_HOST);
    s_lookup_client   = open_client(LOOKUP_HOST);
    s_photoapi_client = open_client(PHOTO_HOST, PHOTO_AGENT);
    // The thumbnails themselves come off a plain http host, so this one never
    // negotiates TLS at all.
    s_photo_client = open_client("http://t.plnspttrs.net");
    ESP_RETURN_ON_FALSE(s_feed_client != nullptr && s_lookup_client != nullptr &&
                            s_photoapi_client != nullptr && s_photo_client != nullptr,
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

    if (!active) {
        s_drop_cache.store(true, std::memory_order_relaxed);
    }

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
    // Snapped to a coarse grid before it is stored, so neither the feed's url
    // nor anything drawn from it carries the actual address. A hundredth of a
    // degree is about a kilometre of latitude and rather less of longitude,
    // which on an eighty kilometre scope is a pixel or two -- and the panel
    // ends up on a lattice point shared with everyone else nearby rather than
    // on its own doorstep.
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
    out.range_km = s_range_km.load(std::memory_order_relaxed);
    out.ok       = s_ok;
    out.age_s    = s_fetched_us == 0
                       ? -1
                       : static_cast<int>((esp_timer_get_time() - s_fetched_us) / 1000000);
    xSemaphoreGive(s_lock);
}

}  // namespace radar
