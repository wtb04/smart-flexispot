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

#include <cstdio>
#include <cstring>

namespace radar {
namespace {

constexpr char TAG[] = "radar";

// adsb.fi's open feed: no account, no key, and it returns the distance and
// bearing from the point asked about, which saves doing it here.
constexpr char FEED_HOST[]  = "https://opendata.adsb.fi";
// Who is flying it and where it is going. Also keyless, and asked only about
// whatever aircraft has been tapped.
constexpr char LOOKUP_HOST[] = "https://api.adsbdb.com";
constexpr char AGENT[] = "tab5-panel";

// Asked for in nautical miles because that is what the feed takes, shown in
// kilometres because that is what the rings are labelled in.
constexpr int RANGE_KM = 80;
constexpr int RANGE_NM = 44;

// Big enough for the thumbnails adsbdb points at, which run to a few hundred
// pixels across.
constexpr int PHOTO_MAX_W = 320;
constexpr int PHOTO_MAX_H = 240;

// The feed asks for no more than one request a second; the fast rate is a
// tenth of that and still quicker than aircraft cross forty miles of scope.
// The slow one only has to keep the page from opening on nothing.
// The home position lands while the broker, the socket and Bluetooth are all
// still coming up, and a TLS handshake thrown in on top of that has failed for
// want of internal memory. A few seconds is enough for the rest to settle.
constexpr std::int64_t FIRST_FETCH_DELAY_US = 6 * 1000000LL;

constexpr std::int64_t POLL_ACTIVE_US = 10 * 1000000LL;
constexpr std::int64_t POLL_IDLE_US   = 60 * 1000000LL;

// Forty nautical miles over a busy corner of Europe is about 25 kB; this is
// room for a far denser sky. It lives in PSRAM, where it is not missed.
constexpr std::size_t BODY_MAX = 96 * 1024;

// An https handshake and the mbedtls session live on this stack, and the
// certificate bundle is walked on it too.
constexpr std::uint32_t TASK_STACK    = 16384;
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

char        *s_body     = nullptr;
std::size_t  s_body_len = 0;

SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_ctrl;

Aircraft     s_list[kMaxAircraft];
int          s_count      = 0;

// Static rather than automatic: an Aircraft is sixty-odd bytes and forty of
// them is two and a half kilobytes, which is not something to put on a stack
// that is already carrying a TLS session.
Aircraft s_scratch[kMaxAircraft];
Snapshot s_published;
bool         s_ok         = false;
std::int64_t s_fetched_us = 0;

UpdateHandler  s_on_update  = nullptr;
DetailsHandler s_on_details = nullptr;
PhotoHandler   s_on_photo   = nullptr;

std::uint16_t *s_photo    = nullptr;
int            s_photo_w  = 0;
int            s_photo_h  = 0;

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

// `what` rather than the url: the feed's url has the panel's own coordinates
// in its path, and these lines are readable on the diagnostics page.
bool get(const char *url, const char *what)
{
    esp_http_client_config_t cfg = {};
    cfg.url                      = url;
    cfg.event_handler            = on_event;
    cfg.timeout_ms               = 10000;
    cfg.crt_bundle_attach        = esp_crt_bundle_attach;
    cfg.user_agent               = AGENT;
    cfg.buffer_size              = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }

    s_body_len = 0;
    s_body[0]  = '\0';

    const esp_err_t err    = esp_http_client_perform(client);
    const int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        // A callsign the database has never heard of answers 404, which is an
        // ordinary answer rather than something going wrong.
        if (status == 404) {
            ESP_LOGD(TAG, "%s not known", what);
        } else {
            ESP_LOGW(TAG, "%s unreachable: %s, http %d", what, esp_err_to_name(err), status);
        }
        return false;
    }
    return true;
}

bool fetch(float lat, float lon)
{
    char url[128];
    std::snprintf(url, sizeof(url), "%s/api/v2/lat/%.4f/lon/%.4f/dist/%d", FEED_HOST,
                  static_cast<double>(lat), static_cast<double>(lon), RANGE_NM);
    if (!get(url, "feed")) {
        return false;
    }

    const int count = parse(s_body, s_body_len, s_scratch, kMaxAircraft);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::memcpy(s_list, s_scratch, sizeof(Aircraft) * static_cast<std::size_t>(count));
    s_count      = count;
    s_ok         = true;
    s_fetched_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "%d aircraft within %d km, %u bytes", count, RANGE_KM,
             static_cast<unsigned>(s_body_len));

    if (s_on_update != nullptr) {
        snapshot(s_published);
        s_on_update(s_published);
    }
    return true;
}

void fetch_photo(const char *hex, const Details &details)
{
    if (s_photo == nullptr || details.photo_url[0] == '\0') {
        if (s_on_photo != nullptr) {
            s_on_photo(hex, nullptr, 0, 0);
        }
        return;
    }

    int width  = 0;
    int height = 0;
    if (get(details.photo_url, "photo") &&
        media::decode_image(s_body, s_body_len, s_photo, PHOTO_MAX_W, PHOTO_MAX_H, width,
                            height)) {
        s_photo_w = width;
        s_photo_h = height;
        ESP_LOGI(TAG, "%s: photo %dx%d", hex, width, height);
        if (s_on_photo != nullptr) {
            s_on_photo(hex, s_photo, width, height);
        }
        return;
    }
    if (s_on_photo != nullptr) {
        s_on_photo(hex, nullptr, 0, 0);
    }
}

void look_up(const char *hex, const char *callsign)
{
    Details details{};
    char    url[160];

    // The route first: it is the headline, and showing it while the rest is
    // still coming is better than showing nothing until all of it has.
    if (callsign[0] != '\0') {
        std::snprintf(url, sizeof(url), "%s/v0/callsign/%s", LOOKUP_HOST, callsign);
        if (get(url, "route")) {
            parse_route(s_body, s_body_len, details);
        }
    }
    if (s_on_details != nullptr) {
        s_on_details(hex, details);
    }

    std::snprintf(url, sizeof(url), "%s/v0/aircraft/%s", LOOKUP_HOST, hex);
    if (get(url, "aircraft")) {
        parse_aircraft(s_body, s_body_len, details);
    }

    ESP_LOGI(TAG, "%s: %s%s%s", hex, details.has_aircraft ? details.model : "unknown type",
             details.has_route ? ", " : "", details.has_route ? details.origin_code : "");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_details = details;
    xSemaphoreGive(s_lock);

    if (s_on_details != nullptr) {
        s_on_details(hex, details);
    }
    fetch_photo(hex, details);
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

        // A tap is waiting on an answer, so it goes before the next sweep.
        if (pending) {
            look_up(hex, callsign);
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

        const std::int64_t due = active ? POLL_ACTIVE_US : POLL_IDLE_US;
        if (last_fetch == 0 || esp_timer_get_time() - last_fetch >= due) {
            if (!fetch(lat, lon)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_ok = false;
                xSemaphoreGive(s_lock);
            }
            last_fetch = esp_timer_get_time();
        }

        // A notification cuts the wait short, which is how opening the page
        // gets a reading straight away rather than up to a minute later.
        const std::int64_t waited = esp_timer_get_time() - last_fetch;
        const std::int64_t rest   = waited >= due ? 1000 : (due - waited) / 1000;
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
    s_home_lat       = lat;
    s_home_lon       = lon;
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
    out.range_km = RANGE_KM;
    out.ok       = s_ok;
    out.age_s    = s_fetched_us == 0
                       ? -1
                       : static_cast<int>((esp_timer_get_time() - s_fetched_us) / 1000000);
    xSemaphoreGive(s_lock);
}

}  // namespace radar
