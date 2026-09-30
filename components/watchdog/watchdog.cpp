#include "watchdog.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <cstdlib>
#include <mutex>

namespace watchdog {
namespace {
constexpr char         TAG[]    = "watchdog";
constexpr std::int64_t LOOK_US  = 1000 * 1000;
constexpr int          GRACE_MS = 300;  // for the log to reach the buffer the restart keeps

Core                s_core;
std::mutex          s_lock;
esp_timer_handle_t  s_timer = nullptr;

void look(void *)
{
    Overdue found;
    bool    stuck = false;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        stuck = s_core.overdue(esp_timer_get_time(), found);
    }
    if (!stuck) {
        return;
    }
    ESP_LOGE(TAG, "%s stuck on %s for %d s: restarting", found.who.c_str(), found.what.c_str(),
             found.for_ms / 1000);
    esp_timer_stop(s_timer);
    const esp_timer_create_args_t args{
        .callback = [](void *) { std::abort(); }, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
        .name = "watchdog_abort", .skip_unhandled_events = false};
    esp_timer_handle_t later = nullptr;
    if (esp_timer_create(&args, &later) != ESP_OK || esp_timer_start_once(later, GRACE_MS * 1000) != ESP_OK) {
        std::abort();
    }
}

void start()
{
    const esp_timer_create_args_t args{.callback = look, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                       .name = "watchdog", .skip_unhandled_events = true};
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, LOOK_US);
    } else {
        ESP_LOGE(TAG, "no timer to look with");
    }
}
}  // namespace

Beat add(const char *who, int limit_ms)
{
    static const bool started = (start(), true);
    (void)started;
    std::lock_guard<std::mutex> hold(s_lock);
    return s_core.add(who, limit_ms);
}

void busy(Beat beat, const char *what)
{
    std::lock_guard<std::mutex> hold(s_lock);
    s_core.busy(beat, what, esp_timer_get_time());
}

void idle(Beat beat)
{
    std::lock_guard<std::mutex> hold(s_lock);
    s_core.idle(beat);
}

}  // namespace watchdog
