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

constexpr char FEED_HOST[]  = "http://api.adsb.lol";
constexpr char LOOKUP_HOST[] = "https://api.adsbdb.com";

constexpr char PHOTO_HOST[]  = "https://api.planespotters.net";
constexpr char PHOTO_AGENT[] = "tab5-panel (+https://woutertenbrinke.nl)";
constexpr char AGENT[]       = "tab5-panel";

constexpr int    RANGE_DEFAULT_KM = 160;

// The home position is snapped to this grid before it reaches a url or the
// screen, so neither carries the actual address.
constexpr float HOME_GRID_DEG = 0.01f;
constexpr float  NM_PER_KM        = 0.539957f;
std::atomic<int>  s_range_km{RANGE_DEFAULT_KM};
std::atomic<bool> s_force_fetch{false};

constexpr int PHOTO_MAX_W = 320;
constexpr int PHOTO_MAX_H = 240;

constexpr std::int64_t FIRST_FETCH_DELAY_US = 6 * 1000000LL;

constexpr std::int64_t POLL_ACTIVE_US = 5 * 1000000LL;
constexpr std::int64_t POLL_IDLE_US   = 60 * 1000000LL;

constexpr std::size_t BODY_MAX = 192 * 1024;

constexpr std::uint32_t TASK_STACK    = 8192;  // measured: uses 3.1 KB; the TLS handshake runs on it
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

char        *s_body     = nullptr;
std::size_t  s_body_len = 0;

constexpr std::int64_t IDLE_CLOSE_US = 20 * 1000000LL;

constexpr std::int64_t PHOTO_IDLE_US = 5 * 1000000LL;

esp_http_client_handle_t s_feed_client     = nullptr;
esp_http_client_handle_t s_lookup_client   = nullptr;
esp_http_client_handle_t s_photoapi_client = nullptr;
esp_http_client_handle_t s_photo_client    = nullptr;

std::int64_t s_lookup_at_us = 0;
bool         s_lookups_open = false;
std::int64_t s_photo_at_us  = 0;
bool         s_photo_open   = false;

constexpr int PREFETCH_PER_SWEEP = 3;

std::atomic<bool> s_drop_cache{false};

constexpr int CACHE_SIZE = 24;

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

Aircraft    *s_list = nullptr;
int          s_count      = 0;

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

esp_http_client_handle_t open_client(const char *url, const char *agent = AGENT)
{
    esp_http_client_config_t cfg = {};
    cfg.url                      = url;
    cfg.event_handler            = on_event;
    cfg.timeout_ms               = 10000;
    cfg.user_agent               = agent;
    cfg.buffer_size              = 2048;
    cfg.keep_alive_enable        = true;
    // Only where TLS can actually be negotiated: two of these hosts are plain
    // http, and a root store there is internal memory spent on nothing.
    if (std::strncmp(url, "https://", 8) == 0) {
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

bool fetch_details(const char *hex, const char *callsign, Details &out)
{
    char url[192];
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
        s_lookups_open = false;
    }
}

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

void warm_photo(const char *hex, const char *callsign)
{
    CacheEntry *entry = cache_find(hex, callsign);
    if (entry == nullptr || entry->details.photo_checked) {
        return;
    }
    resolve_photo(hex, entry->details);
}

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

        if (pending) {
            look_up(hex, callsign, true);
            continue;
        }

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

        const std::int64_t waited = esp_timer_get_time() - last_fetch;
        std::int64_t       rest   = waited >= due ? 1000 : (due - waited) / 1000;
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

    s_feed_client     = open_client(FEED_HOST);
    s_lookup_client   = open_client(LOOKUP_HOST);
    s_photoapi_client = open_client(PHOTO_HOST, PHOTO_AGENT);
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
