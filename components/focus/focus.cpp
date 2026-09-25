#include "focus.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace focus {
namespace {
constexpr char TAG[] = "focus";

portMUX_TYPE       s_lock  = portMUX_INITIALIZER_UNLOCKED;
Plan               s_plan{};
State              s_state{};
esp_timer_handle_t s_timer = nullptr;
ChangeHandler      s_on_change = nullptr;

// Wakes when the running part is due to end, and not before: nothing ticks.
void arm(const State &state)
{
    esp_timer_stop(s_timer);
    if (state.running) {
        const std::int64_t wait_ms = state.ends_at - now_ms();
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_start_once(s_timer, static_cast<std::uint64_t>(wait_ms > 0 ? wait_ms : 0) * 1000));
    }
}

void settle(const State &next, bool finished)
{
    portENTER_CRITICAL(&s_lock);
    s_state = next;
    portEXIT_CRITICAL(&s_lock);
    arm(next);
    ESP_LOGI(TAG, "%s round %d %s", next.phase == Phase::Work        ? "focus"
                                    : next.phase == Phase::Break     ? "break"
                                    : next.phase == Phase::LongBreak ? "long break"
                                                                     : "idle",
             next.round, next.running ? "running" : "paused");
    if (s_on_change != nullptr) {
        s_on_change(next, finished);
    }
}

void ran_out(void *)
{
    settle(after(state(), plan(), now_ms()), true);
}

}  // namespace

esp_err_t start(ChangeHandler on_change)
{
    ESP_RETURN_ON_FALSE(s_timer == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_on_change = on_change;
    const esp_timer_create_args_t args{.callback = ran_out, .name = "focus"};
    return esp_timer_create(&args, &s_timer);
}

void act(Action action)
{
    if (s_timer == nullptr) {
        return;
    }
    const State        now_state = state();
    const Plan         now_plan  = plan();
    const std::int64_t now       = now_ms();
    switch (action) {
        case Action::Toggle: settle(toggled(now_state, now_plan, now), false); break;
        case Action::Skip:   settle(after(now_state, now_plan, now), false); break;
        case Action::Reset:  settle(State{}, false); break;
    }
}

State state()
{
    portENTER_CRITICAL(&s_lock);
    const State copy = s_state;
    portEXIT_CRITICAL(&s_lock);
    return copy;
}

Plan plan()
{
    portENTER_CRITICAL(&s_lock);
    const Plan copy = s_plan;
    portEXIT_CRITICAL(&s_lock);
    return copy;
}

void set_plan(const Plan &plan)
{
    portENTER_CRITICAL(&s_lock);
    s_plan = plan;
    portEXIT_CRITICAL(&s_lock);
    if (s_on_change != nullptr) {
        s_on_change(state(), false);  // so whatever shows the plan shows the new one
    }
}

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

}  // namespace focus
