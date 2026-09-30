#include "jobs.h"

#include "app_state.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "job_core.h"
#include "watchdog.h"

#include <algorithm>

namespace jobs {
namespace {
constexpr char TAG[] = "jobs";

// Quick jobs may write flash, as confirming an update does, which a stack in
// PSRAM cannot be used through; slow ones only fetch and parse.
constexpr std::uint32_t QUICK_STACK   = 6144;
constexpr std::uint32_t SLOW_STACK    = 8192;
// Quick ones above net's workers, whose TLS handshakes kept the clock and the
// rest waiting a second on end; being quick, they hold net up hardly at all.
// Slow ones wait on net anyway.
constexpr UBaseType_t   QUICK_PRIORITY = 4;
constexpr UBaseType_t   SLOW_PRIORITY  = 2;
constexpr BaseType_t    CORE          = 0;  // LVGL has the other
constexpr std::int64_t  LONGEST_SLEEP = 60 * 1000 * 1000;
constexpr int           QUICK_MOST_MS = 100;  // a quick job taking longer holds up the rest
// Past these a job is taken to be stuck: a quick one, or one waiting on a
// fetch, which net gives up on after a minute.
constexpr int           QUICK_STUCK_MS = 10 * 1000;
constexpr int           SLOW_STUCK_MS  = 3 * 60 * 1000;

Core              s_core;
SemaphoreHandle_t s_lock       = nullptr;
TaskHandle_t      s_workers[2] = {};

StaticTask_t s_quick_ctrl;
StackType_t  s_quick_stack[QUICK_STACK];
StaticTask_t s_slow_ctrl;

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

void wake_all()
{
    for (TaskHandle_t worker : s_workers) {
        if (worker != nullptr) {
            xTaskNotifyGive(worker);
        }
    }
}

[[noreturn]] void worker_task(void *arg)
{
    const auto            lane = static_cast<Lane>(reinterpret_cast<std::intptr_t>(arg));
    const watchdog::Beat beat = lane == Lane::Quick ? watchdog::add("jobs", QUICK_STUCK_MS)
                                                    : watchdog::add("jobs_slow", SLOW_STUCK_MS);
    for (;;) {
        Job                     job = kNoJob;
        bool                    got = false;
        std::function<Result()> run;
        const char             *what = "";
        std::int64_t            now  = esp_timer_get_time();
        std::int64_t            due  = Core::kNeverUs;
        {
            Lock hold;
            s_core.conditions(app::get(app::Fact::Online), app::get(app::Fact::ScreenOn));
            got = s_core.next(lane, now, job);
            if (got) {
                run  = s_core.spec(job).run;
                what = s_core.spec(job).name;
            } else {
                due = s_core.due(lane, now);
            }
        }
        if (got) {
            watchdog::busy(beat, what);
            const Result result = run ? run() : done();
            watchdog::idle(beat);
            const std::int64_t end     = esp_timer_get_time();
            const int          took_ms = static_cast<int>((end - now) / 1000);
            int                most    = 0;
            {
                Lock hold;
                most = s_core.status(job, end).most_ms;
                s_core.finish(job, result, took_ms, end);
            }
            if (lane == Lane::Quick && took_ms > QUICK_MOST_MS && took_ms > most) {
                ESP_LOGW(TAG, "%s took %d ms, holding up the other quick jobs", what, took_ms);
            }
            continue;
        }
        const std::int64_t wait_us = std::clamp<std::int64_t>(due - esp_timer_get_time(), 1000, LONGEST_SLEEP);
        ulTaskNotifyTake(pdTRUE, std::max<TickType_t>(1, pdMS_TO_TICKS(wait_us / 1000)));
    }
}

void start_workers()
{
    static StaticSemaphore_t lock_ctrl;
    s_lock = xSemaphoreCreateMutexStatic(&lock_ctrl);
    s_workers[0] = xTaskCreateStaticPinnedToCore(worker_task, "jobs", QUICK_STACK,
                                                 reinterpret_cast<void *>(Lane::Quick), QUICK_PRIORITY,
                                                 s_quick_stack, &s_quick_ctrl, CORE);
    auto *slow_stack = static_cast<StackType_t *>(
        heap_caps_malloc(SLOW_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (slow_stack != nullptr) {
        s_workers[1] = xTaskCreateStaticPinnedToCore(worker_task, "jobs_slow", SLOW_STACK,
                                                     reinterpret_cast<void *>(Lane::Slow), SLOW_PRIORITY,
                                                     slow_stack, &s_slow_ctrl, CORE);
    }
    if (s_workers[0] == nullptr || s_workers[1] == nullptr) {
        ESP_LOGE(TAG, "workers would not start");
    }
    app::watch(app::Fact::Online, [](bool) { wake_all(); });
    app::watch(app::Fact::ScreenOn, [](bool) { wake_all(); });
}
}  // namespace

Job add(Spec spec)
{
    static const bool started = (start_workers(), true);
    (void)started;
    Job job = kNoJob;
    {
        Lock hold;
        job = s_core.add(std::move(spec), esp_timer_get_time());
    }
    wake_all();
    return job;
}

void poke(Job job)
{
    if (s_lock == nullptr || job == kNoJob) {
        return;
    }
    {
        Lock hold;
        s_core.poke(job, esp_timer_get_time());
    }
    wake_all();
}

int count()
{
    if (s_lock == nullptr) {
        return 0;
    }
    Lock hold;
    return s_core.count();
}

const char *name(Job job)
{
    if (s_lock == nullptr || job < 0) {
        return "";
    }
    Lock hold;
    return job < s_core.count() ? s_core.spec(job).name : "";
}

Status status(Job job)
{
    if (s_lock == nullptr) {
        return {};
    }
    Lock hold;
    return job >= 0 && job < s_core.count() ? s_core.status(job, esp_timer_get_time()) : Status{};
}

}  // namespace jobs
