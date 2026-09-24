#include "travel.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "travel_secrets.h"
#include "wifi.h"

#ifndef TRAVEL_API_KEY
#define TRAVEL_API_KEY ""
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace travel {
namespace {
constexpr char TAG[] = "travel";

// The answer is a few hundred bytes: the whole point of the service is that the
// panel never meets a journey planner's own JSON.
constexpr std::size_t BODY_MAX = 8 * 1024;

// Departures move, but only the imminent ones move in a way anybody acts on.
// Far from the event the train is the whole answer and it hardly changes, so
// asking every few minutes all evening would be asking for nothing.
constexpr std::int64_t NEAR_REFRESH_US = 180 * 1000000LL;
constexpr std::int64_t FAR_REFRESH_US  = 20 * 60 * 1000000LL;
constexpr std::int64_t NEAR_SECONDS    = 90 * 60;

constexpr std::uint32_t TASK_STACK    = 5120;
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_ctrl;

char       *s_body     = nullptr;
std::size_t s_body_len = 0;

Option *s_options = nullptr;
int     s_count   = 0;
bool    s_ok      = false;

std::atomic<std::int64_t> s_wanted{0};
std::atomic<Place>        s_place{Place::Study};
Place                     s_asked_place = Place::Study;
std::int64_t              s_asked_for  = 0;
std::int64_t              s_asked_at   = 0;

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

bool fetch(std::int64_t arrive_by, Place place)
{
    char url[176];
    std::snprintf(url, sizeof(url), "%s/v1/leave?arriveBy=%lld%s", TRAVEL_HOST,
                  static_cast<long long>(arrive_by), place == Place::Work ? "&to=work" : "");

    esp_http_client_config_t cfg{};
    cfg.url           = url;
    cfg.event_handler = on_event;
    cfg.timeout_ms    = 15000;
    cfg.buffer_size   = 1024;
    if (std::strncmp(TRAVEL_HOST, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    if (TRAVEL_API_KEY[0] != '\0') {
        esp_http_client_set_header(client, "X-Api-Key", TRAVEL_API_KEY);
    }

    s_body_len = 0;
    s_body[0]  = '\0';
    const std::int64_t began  = esp_timer_get_time();
    const esp_err_t    err    = esp_http_client_perform(client);
    const int          status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGW(TAG, "%s", err == ESP_OK ? "refused" : esp_err_to_name(err));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ok = false;
        xSemaphoreGive(s_lock);
        return false;
    }

    static Option scratch[kOptionsMax];
    const int     found = parse(s_body, s_body_len, scratch, kOptionsMax);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    std::memcpy(s_options, scratch, sizeof(Option) * static_cast<std::size_t>(found));
    s_count = found;
    s_ok    = true;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "%d option%s in %d ms", found, found == 1 ? "" : "s",
             static_cast<int>((esp_timer_get_time() - began) / 1000));
    return true;
}

[[noreturn]] void travel_task(void *)
{
    for (;;) {
        const std::int64_t wanted = s_wanted.load(std::memory_order_relaxed);
        const Place        place  = s_place.load(std::memory_order_relaxed);
        const std::int64_t now    = esp_timer_get_time();

        const auto away = wanted - static_cast<std::int64_t>(std::time(nullptr));
        const auto due  = away < NEAR_SECONDS ? NEAR_REFRESH_US : FAR_REFRESH_US;

        const bool changed = wanted != s_asked_for || place != s_asked_place;
        const bool stale   = wanted != 0 && now - s_asked_at >= due;

        if (wanted != 0 && wifi::connected() && (changed || stale)) {
            s_asked_for   = wanted;
            s_asked_place = place;
            s_asked_at    = now;
            if (fetch(wanted, place) && s_on_update != nullptr) {
                s_on_update();
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

}  // namespace

esp_err_t start(UpdateHandler on_update)
{
    ESP_RETURN_ON_FALSE(s_task == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    if (TRAVEL_HOST[0] == '\0') {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    s_body = static_cast<char *>(heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_options = static_cast<Option *>(
        heap_caps_calloc(kOptionsMax, sizeof(Option), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_lock != nullptr && s_body != nullptr && s_options != nullptr,
                        ESP_ERR_NO_MEM, TAG, "buffers");

    s_on_update = on_update;
    s_task = xTaskCreateStaticPinnedToCore(travel_task, "travel", TASK_STACK, nullptr,
                                           TASK_PRIORITY, s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void want(std::int64_t arrive_by, Place place)
{
#if CONFIG_TRAVEL_FAKE_MINUTES > 0
    if (arrive_by != 0) {
        // Rounded to the minute so that asking again is the same question and
        // does not set off a fetch every time the page redraws.
        const auto soon = static_cast<std::int64_t>(std::time(nullptr)) +
                          CONFIG_TRAVEL_FAKE_MINUTES * 60;
        arrive_by = soon - soon % 60;
    }
#endif
    const bool moved = s_place.exchange(place, std::memory_order_relaxed) != place;
    if (s_wanted.exchange(arrive_by, std::memory_order_relaxed) == arrive_by && !moved) {
        return;
    }
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

int options(Option *out, int capacity)
{
    if (out == nullptr || capacity <= 0 || s_lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int taken = s_count < capacity ? s_count : capacity;
    std::memcpy(out, s_options, sizeof(Option) * static_cast<std::size_t>(taken));
    xSemaphoreGive(s_lock);
    return taken;
}

bool ok()
{
    if (s_lock == nullptr) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool good = s_ok;
    xSemaphoreGive(s_lock);
    return good;
}

}  // namespace travel
