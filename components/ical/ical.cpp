#include "ical.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <ctime>

namespace ical {
namespace {
constexpr char TAG[] = "ical";

constexpr char HOST[] = "https://calendar.example.org";

constexpr const char *FEEDS[kFeedCount] = {"Lectures", "Practicals", "Exams", "Other"};

// A timetable is not a live feed; it changes when somebody edits it, which is
// rarely and never urgently.
constexpr TickType_t POLL_INTERVAL = pdMS_TO_TICKS(30 * 60 * 1000);

// The largest feed is 22 kB today. In PSRAM, where being generous costs nothing.
constexpr std::size_t BODY_MAX = 128 * 1024;

// Nothing is fetched until the clock is right, or every event is filed against
// 1970 and the page shows the wrong things in the wrong order.
constexpr std::time_t CLOCK_SET_AFTER = 1600000000;

// A round that came back with nothing is usually the network still coming up,
// which is not worth half an hour of silence.
constexpr TickType_t RETRY_INTERVAL = pdMS_TO_TICKS(20 * 1000);

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

Event *s_scratch = nullptr;

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

int fetch_feed(int index)
{
    char url[128];
    std::snprintf(url, sizeof(url), "%s/%s", HOST, FEEDS[index]);

    esp_http_client_config_t cfg{};
    cfg.url               = url;
    cfg.event_handler     = on_event;
    cfg.timeout_ms        = 15000;
    cfg.buffer_size       = 2048;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return -1;
    }

    s_body_len   = 0;
    s_body[0]    = '\0';
    esp_err_t err = esp_http_client_perform(client);
    const int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGW(TAG, "%s: %s", FEEDS[index],
                 err == ESP_OK ? "refused" : esp_err_to_name(err));
        return -1;
    }
    return parse(s_body, s_body_len, static_cast<std::uint8_t>(index), s_scratch, kMaxEvents);
}

bool fetch_all()
{
    int gathered = 0;
    for (int i = 0; i < kFeedCount && gathered < kMaxEvents; ++i) {
        const std::int64_t began = esp_timer_get_time();
        const int          n     = fetch_feed(i);
        if (n < 0) {
            continue;
        }
        const int room = kMaxEvents - gathered;
        const int take = n < room ? n : room;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        std::memcpy(s_events + gathered, s_scratch, sizeof(Event) * static_cast<std::size_t>(take));
        gathered += take;
        s_count = gathered;
        xSemaphoreGive(s_lock);

        ESP_LOGI(TAG, "%s: %d events in %d ms", FEEDS[i], take,
                 static_cast<int>((esp_timer_get_time() - began) / 1000));
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = gathered;
    std::sort(s_events, s_events + s_count,
              [](const Event &a, const Event &b) { return a.start < b.start; });
    xSemaphoreGive(s_lock);

    if (s_on_update != nullptr) {
        s_on_update();
    }
    return gathered > 0;
}

[[noreturn]] void ical_task(void *)
{
    for (;;) {
        if (std::time(nullptr) < CLOCK_SET_AFTER || !wifi::connected()) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }
        ulTaskNotifyTake(pdTRUE, fetch_all() ? POLL_INTERVAL : RETRY_INTERVAL);
    }
}

}  // namespace

const char *feed_name(std::uint8_t feed)
{
    return feed < kFeedCount ? FEEDS[feed] : "";
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
    ESP_RETURN_ON_FALSE(s_body != nullptr && s_events != nullptr && s_scratch != nullptr,
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

void refresh()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace ical
