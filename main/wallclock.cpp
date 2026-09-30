#include "wallclock.h"

#include "clock_math.h"

#include "esp_check.h"
#include "esp_log.h"
#include "jobs.h"
#include "ui.h"
#include "units.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace wallclock {
namespace {
constexpr char TAG[] = "clock";

constexpr int TICK_MS = units::kMsPerSecond;

constexpr std::size_t TIME_TEXT_SIZE = sizeof("HH:MM");

std::atomic<bool> s_synced{false};

jobs::Result show_time()
{
    static char       last[TIME_TEXT_SIZE] = {};
    const std::time_t now                  = std::time(nullptr);
    std::tm           local{};
    localtime_r(&now, &local);

    if (rtc::plausible(now)) {
        char text[TIME_TEXT_SIZE];
        std::strftime(text, sizeof(text), "%H:%M", &local);
        if (!s_synced.exchange(true, std::memory_order_relaxed)) {
            ESP_LOGI(TAG, "time set: %s", text);
        }
        if (std::strncmp(text, last, sizeof(text)) != 0) {
            std::snprintf(last, sizeof(last), "%s", text);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_time(text));
        }
    }
    return jobs::done();
}

}  // namespace

esp_err_t start()
{
    jobs::Spec spec;
    spec.name      = TAG;
    spec.period_ms = TICK_MS;
    spec.run       = show_time;
    ESP_RETURN_ON_FALSE(jobs::add(std::move(spec)) != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

bool synced()
{
    return s_synced.load(std::memory_order_relaxed);
}

}  // namespace wallclock
