#include "focus_model.h"

#include "topics.h"
#include "ui_internal.h"

#include "esp_timer.h"

#include <algorithm>
#include <cstdio>

namespace ui::detail {
namespace {
// As last told; until then, the plan as it stands at boot.
Focus s_focus{
    .phase          = FocusPhase::Idle,
    .round          = 0,
    .rounds         = FOCUS_ROUNDS_DEFAULT,
    .running        = false,
    .ends_at_ms     = 0,
    .left_ms        = 0,
    .length_ms      = 0,
    .work_min       = FOCUS_WORK_MIN_DEFAULT,
    .break_min      = FOCUS_BREAK_MIN_DEFAULT,
    .long_break_min = FOCUS_LONG_BREAK_MIN_DEFAULT,
};
}  // namespace

const Focus &focus_state()
{
    return s_focus;
}

void set_focus_state(const Focus &focus)
{
    s_focus = focus;
    publish(Topic::Focus);
}

std::int64_t focus_now_ms()
{
    return esp_timer_get_time() / units::kUsPerMs;
}

std::int64_t whole_seconds_up(std::int64_t ms)
{
    return (ms + units::kMsPerSecond - 1) / units::kMsPerSecond;
}

bool focus_idle(const Focus &focus)
{
    return focus.phase == FocusPhase::Idle;
}

bool focus_resting(const Focus &focus)
{
    return focus.phase == FocusPhase::Break || focus.phase == FocusPhase::LongBreak;
}

bool focus_waiting(const Focus &focus)
{
    return !focus_idle(focus) && !focus.running && focus.left_ms == focus.length_ms;
}

bool focus_paused(const Focus &focus)
{
    return !focus_idle(focus) && !focus.running;
}

std::int32_t focus_left_ms(const Focus &focus)
{
    if (focus_idle(focus)) {
        return focus.work_min * units::kMsPerMinute;
    }
    if (!focus.running) {
        return focus.left_ms;
    }
    return static_cast<std::int32_t>(std::max<std::int64_t>(0, focus.ends_at_ms - focus_now_ms()));
}

std::uint32_t focus_ink(bool resting)
{
    return resting ? theme::green : theme::primary;
}

void focus_clock_text(const Focus &focus, char *out, std::size_t size)
{
    if (focus_waiting(focus)) {
        std::snprintf(out, size, "Ready");
        return;
    }
    const int seconds = static_cast<int>(whole_seconds_up(focus_left_ms(focus)));
    std::snprintf(out, size, "%02d:%02d", seconds / units::kSecondsPerMinute, seconds % units::kSecondsPerMinute);
}

}  // namespace ui::detail
