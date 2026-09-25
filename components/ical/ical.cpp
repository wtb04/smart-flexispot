#include "ical.h"

#include "clock_math.h"

#include "esp_check.h"
#include "ical_secrets.h"

#ifndef ICAL_WORK_URL
#define ICAL_WORK_URL ""
#endif
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "units.h"
#include "wifi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <ctime>

namespace ical {
namespace {
constexpr char TAG[] = "ical";

constexpr char HOST[] = "https://calendar.example.org";

// The timetable's four come from CalendarChanger by name. Work is a shared
// Outlook calendar, of which only the shifts belong here.
struct Feed {
    const char *name;
    const char *url;   // null: HOST/name
    const char *keep;  // when set, only events whose summary starts with it
};
constexpr Feed FEEDS[kFeedCount] = {
    {"Lectures", nullptr, nullptr},
    {"Practicals", nullptr, nullptr},
    {"Exams", nullptr, nullptr},
    {"Other", nullptr, nullptr},
    {"Work", ICAL_WORK_URL, "werk"},
};
static_assert(kWorkFeed < kFeedCount, "the work feed is one of the feeds");

// Case aside, so "Werk" and "Werken" both count.
bool starts_with(const char *text, const char *prefix)
{
    for (; *prefix != '\0'; ++text, ++prefix) {
        if (std::tolower(static_cast<unsigned char>(*text)) != *prefix) {
            return false;
        }
    }
    return true;
}

// A timetable is not a live feed; it changes when somebody edits it, which is
// rarely and never urgently.
constexpr TickType_t POLL_INTERVAL = pdMS_TO_TICKS(30 * units::kMsPerMinute);

// The work calendar is 158 kB, the timetable feeds 22 kB at most. In PSRAM.
constexpr std::size_t BODY_MAX = 256 * units::kBytesPerKiB;

// A feed that did not come back is usually the network still coming up, or a
// server hanging up early; neither is worth half an hour of silence.
constexpr TickType_t RETRY_INTERVAL     = pdMS_TO_TICKS(20 * units::kMsPerSecond);
constexpr TickType_t MAX_RETRY_INTERVAL = pdMS_TO_TICKS(5 * units::kMsPerMinute);
constexpr int        MAX_BACKOFF_STEPS  = 4;

constexpr TickType_t NOT_READY_WAIT = pdMS_TO_TICKS(2 * units::kMsPerSecond);

constexpr std::size_t URL_SIZE         = 256;
constexpr int         HTTP_TIMEOUT_MS  = 15 * units::kMsPerSecond;
constexpr int         HTTP_BUFFER_SIZE = 2 * units::kBytesPerKiB;
constexpr int         HTTP_OK          = 200;

constexpr std::uint32_t TASK_STACK    = 6144;
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_ctrl;

char       *s_body     = nullptr;
std::size_t s_body_len = 0;

Event *s_events = nullptr;
int    s_count  = 0;

Event *s_scratch  = nullptr;
Event *s_building = nullptr;

UpdateHandler s_on_update = nullptr;

esp_err_t on_event(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || s_body == nullptr) {
        return ESP_OK;
    }
    const auto room = BODY_MAX - 1 - s_body_len;
    const auto take = static_cast<std::size_t>(event->data_len) < room
                          ? static_cast<std::size_t>(event->data_len)
                          : room;
    std::memcpy(s_body + s_body_len, event->data, take);
    s_body_len += take;
    s_body[s_body_len] = '\0';
    return ESP_OK;
}

int keep_only(Event *events, int count, const char *prefix)
{
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        if (starts_with(events[i].summary, prefix)) {
            events[kept++] = events[i];
        }
    }
    return kept;
}

int fetch_feed(int index)
{
    const Feed &feed = FEEDS[index];
    if (feed.url != nullptr && feed.url[0] == '\0') {
        return -1;  // not configured
    }
    char url[URL_SIZE];
    if (feed.url != nullptr) {
        std::snprintf(url, sizeof(url), "%s", feed.url);
    } else {
        std::snprintf(url, sizeof(url), "%s/%s", HOST, feed.name);
    }

    esp_http_client_config_t cfg{};
    cfg.url               = url;
    cfg.event_handler     = on_event;
    cfg.timeout_ms        = HTTP_TIMEOUT_MS;
    cfg.buffer_size       = HTTP_BUFFER_SIZE;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return -1;
    }

    s_body_len             = 0;
    s_body[0]              = '\0';
    const esp_err_t err    = esp_http_client_perform(client);
    const int       status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (status != HTTP_OK) {
        ESP_LOGW(TAG, "%s: %s", feed.name,
                 err == ESP_OK ? "refused" : esp_err_to_name(err));
        return -1;
    }
    const int count =
        parse(s_body, s_body_len, static_cast<std::uint8_t>(index), s_scratch, kMaxEvents);
    return feed.keep != nullptr ? keep_only(s_scratch, count, feed.keep) : count;
}

int keep_previous(int feed, int &built)
{
    int kept = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int e = 0; e < s_count && built < kMaxEvents; ++e) {
        if (s_events[e].feed == feed) {
            s_building[built++] = s_events[e];
            ++kept;
        }
    }
    xSemaphoreGive(s_lock);
    return kept;
}

// Each round is built aside and swapped in whole, and a feed that fails keeps
// its last events rather than vanishing from the page until the next round.
bool fetch_all()
{
    int  built  = 0;
    bool all_ok = true;
    for (int i = 0; i < kFeedCount; ++i) {
        const std::int64_t began = esp_timer_get_time();
        const int          n     = fetch_feed(i);
        if (n < 0) {
            all_ok         = false;
            const int kept = keep_previous(i, built);
            ESP_LOGW(TAG, "%s: keeping the %d events from before", FEEDS[i].name, kept);
            continue;
        }
        const int take = std::min(n, kMaxEvents - built);
        std::memcpy(s_building + built, s_scratch, sizeof(Event) * static_cast<std::size_t>(take));
        built += take;
        ESP_LOGI(TAG, "%s: %d events in %d ms", FEEDS[i].name, take,
                 static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs));
    }

    std::sort(s_building, s_building + built,
              [](const Event &a, const Event &b) { return a.start < b.start; });
    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::swap(s_events, s_building);
    s_count = built;
    xSemaphoreGive(s_lock);

    if (s_on_update != nullptr) {
        s_on_update();
    }
    return all_ok;
}

// Each failed round waits twice as long as the last, up to five minutes: a
// host that is down gets a handful of handshakes, not a stream.
TickType_t wait_after(int failures)
{
    if (failures == 0) {
        return POLL_INTERVAL;
    }
    return std::min<TickType_t>(RETRY_INTERVAL << (failures - 1), MAX_RETRY_INTERVAL);
}

[[noreturn]] void ical_task(void *)
{
    for (;;) {
        // Nothing is fetched until the clock is right, or every event is filed
        // against 1970 and the page shows the wrong things in the wrong order.
        if (!rtc::plausible(std::time(nullptr)) || !wifi::connected()) {
            ulTaskNotifyTake(pdTRUE, NOT_READY_WAIT);
            continue;
        }
        static int failures = 0;
        failures = fetch_all() ? 0 : std::min(failures + 1, MAX_BACKOFF_STEPS);
        ulTaskNotifyTake(pdTRUE, wait_after(failures));
    }
}

}  // namespace

const char *feed_name(std::uint8_t feed)
{
    return feed < kFeedCount ? FEEDS[feed].name : "";
}

esp_err_t start(UpdateHandler on_update)
{
    ESP_RETURN_ON_FALSE(s_task == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, TAG, "lock");

    s_body = static_cast<char *>(heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_events = static_cast<Event *>(
        heap_caps_calloc(kMaxEvents, sizeof(Event), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_scratch = static_cast<Event *>(
        heap_caps_calloc(kMaxEvents, sizeof(Event), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_building = static_cast<Event *>(
        heap_caps_calloc(kMaxEvents, sizeof(Event), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_body != nullptr && s_events != nullptr && s_scratch != nullptr &&
                            s_building != nullptr,
                        ESP_ERR_NO_MEM, TAG, "buffers");

    s_on_update = on_update;
    s_task      = xTaskCreateStaticPinnedToCore(ical_task, "ical", TASK_STACK, nullptr,
                                                TASK_PRIORITY, s_task_stack, &s_task_ctrl,
                                                TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

int upcoming(Event *out, int capacity)
{
    if (out == nullptr || capacity <= 0 || s_lock == nullptr) {
        return 0;
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int written = 0;
    for (int i = 0; i < s_count && written < capacity; ++i) {
        if (s_events[i].end >= now) {
            out[written++] = s_events[i];
        }
    }
    xSemaphoreGive(s_lock);
    return written;
}

int between(std::int64_t from, std::int64_t to, Event *out, int capacity)
{
    if (out == nullptr || capacity <= 0 || s_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int written = 0;
    for (int i = 0; i < s_count && written < capacity; ++i) {
        if (s_events[i].end > from && s_events[i].start < to) {
            out[written++] = s_events[i];
        }
    }
    xSemaphoreGive(s_lock);
    return written;
}

void refresh()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace ical
