#include "travel.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "units.h"
#include "jobs.h"

#if __has_include("travel_secrets.h")
#include "travel_secrets.h"
#endif
#ifndef TRAVEL_HOST
#define TRAVEL_HOST ""
#endif
#ifndef TRAVEL_API_KEY
#define TRAVEL_API_KEY ""
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace travel {
namespace {
constexpr char TAG[] = "travel";

// The answer is a few hundred bytes: the whole point of the service is that the
// panel never meets a journey planner's own JSON.
constexpr std::size_t BODY_MAX = 8 * units::kBytesPerKiB;

// Departures move, but only the imminent ones move in a way anybody acts on.
// Far from the event the train is the whole answer and it hardly changes, so
// asking every few minutes all evening would be asking for nothing.
constexpr std::int64_t NEAR_REFRESH_US = 3 * units::kUsPerMinute;
constexpr std::int64_t FAR_REFRESH_US  = 20 * units::kUsPerMinute;
constexpr std::int64_t NEAR_SECONDS    = 90 * units::kSecondsPerMinute;

constexpr int CHECK_MS = 5 * units::kMsPerSecond;

constexpr std::size_t URL_SIZE         = 176;
constexpr int         HTTP_TIMEOUT_MS  = 15 * units::kMsPerSecond;

jobs::Job s_job = jobs::kNoJob;

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

net::Host s_host = net::kNoHost;

void add_host()
{
    static const std::string headers = std::string("X-Api-Key: ") + TRAVEL_API_KEY;
    net::HostConfig config;
    config.name        = "travel";
    config.base        = TRAVEL_HOST;
    config.headers     = TRAVEL_API_KEY[0] != '\0' ? headers.c_str() : "";
    config.timeout_ms  = HTTP_TIMEOUT_MS;
    config.connections = 1;
    config.idle_ms     = units::kMsPerMinute;
    config.retry       = net::Retry{1, units::kMsPerSecond, 200, false};
    s_host             = net::add_host(config);
}

void set_ok(bool ok)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ok = ok;
    xSemaphoreGive(s_lock);
}

bool fetch(std::int64_t arrive_by, Place place)
{
    char path[URL_SIZE];
    std::snprintf(path, sizeof(path), "/v1/leave?arriveBy=%lld%s", static_cast<long long>(arrive_by),
                  place == Place::Work ? "&to=work" : "");

    net::Request request;
    request.host     = s_host;
    request.path     = path;
    request.priority = net::Priority::Now;
    request.key      = "leave";
    request.dedupe   = net::Dedupe::Replace;
    request.max_body = BODY_MAX - 1;
    request.what     = "journey";
    const std::int64_t began = esp_timer_get_time();
    const net::Fetched got   = net::fetch(std::move(request), s_body, BODY_MAX);
    s_body_len               = got.length;
    if (!got.ok()) {
        ESP_LOGW(TAG, "%s", got.outcome == net::Outcome::Answered ? "refused" : "not answered");
        set_ok(false);
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
             static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs));
    return true;
}

jobs::Result check()
{
    const std::int64_t wanted = s_wanted.load(std::memory_order_relaxed);
    const Place        place  = s_place.load(std::memory_order_relaxed);
    const std::int64_t now    = esp_timer_get_time();

    const auto away = wanted - static_cast<std::int64_t>(std::time(nullptr));
    const auto due  = away < NEAR_SECONDS ? NEAR_REFRESH_US : FAR_REFRESH_US;

    const bool changed = wanted != s_asked_for || place != s_asked_place;
    const bool stale   = wanted != 0 && now - s_asked_at >= due;

    if (wanted != 0 && (changed || stale)) {
        s_asked_for   = wanted;
        s_asked_place = place;
        s_asked_at    = now;
        if (fetch(wanted, place) && s_on_update != nullptr) {
            s_on_update();
        }
    }
    return jobs::done();
}

}  // namespace

esp_err_t start(UpdateHandler on_update)
{
    ESP_RETURN_ON_FALSE(s_job == jobs::kNoJob, ESP_ERR_INVALID_STATE, TAG, "already started");
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
    add_host();
    jobs::Spec spec;
    spec.name      = TAG;
    spec.lane      = jobs::Lane::Slow;
    spec.period_ms = CHECK_MS;
    spec.online    = true;
    spec.run       = check;
    s_job          = jobs::add(std::move(spec));
    ESP_RETURN_ON_FALSE(s_job != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

void want(std::int64_t arrive_by, Place place)
{
#if CONFIG_TRAVEL_FAKE_MINUTES > 0
    if (arrive_by != 0) {
        // Rounded to the minute so that asking again is the same question and
        // does not set off a fetch every time the page redraws.
        const auto soon = static_cast<std::int64_t>(std::time(nullptr)) +
                          CONFIG_TRAVEL_FAKE_MINUTES * units::kSecondsPerMinute;
        arrive_by = soon - soon % units::kSecondsPerMinute;
    }
#endif
    const bool moved = s_place.exchange(place, std::memory_order_relaxed) != place;
    if (s_wanted.exchange(arrive_by, std::memory_order_relaxed) == arrive_by && !moved) {
        return;
    }
    jobs::poke(s_job);
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
