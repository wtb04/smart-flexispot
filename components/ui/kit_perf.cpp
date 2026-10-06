#include "kit.h"

#include <algorithm>

namespace ui::kit::perf {
namespace {

Clock s_clock = nullptr;
bool  s_started = false;

struct Acc {
    std::uint32_t frames = 0;
    std::uint64_t total_us = 0, worst_us = 0, from_us = 0;
    void add(std::uint64_t us)
    {
        ++frames;
        total_us += us;
        worst_us = std::max(worst_us, us);
    }
    Stats stats(std::uint64_t to_us) const
    {
        Stats s;
        s.frames   = frames;
        s.avg_ms   = frames == 0 ? 0.0f : static_cast<float>(total_us) / static_cast<float>(frames) / 1000.0f;
        s.worst_ms = static_cast<float>(worst_us) / 1000.0f;
        s.span_ms  = static_cast<std::uint32_t>((to_us - from_us) / 1000);
        return s;
    }
};

Acc           s_second, s_phase;
Stats         s_last;
bool          s_phase_on = false;
std::uint64_t s_render_at = 0;
bool          s_rendering = false;
std::uint16_t s_each[48];
int           s_each_n = 0;

// A frame is from the start of rendering until the flush has gone, so the
// panel's rotation into its frame buffer counts too.
void on_display(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RENDER_START) {
        if (!s_rendering) {
            s_rendering = true;
            s_render_at = now_us();
        }
    } else if (code == LV_EVENT_REFR_READY && s_rendering) {
        s_rendering = false;
        const std::uint64_t took = now_us() - s_render_at;
        s_second.add(took);
        if (s_phase_on) {
            s_phase.add(took);
            if (s_each_n < 48) {
                s_each[s_each_n++] = static_cast<std::uint16_t>(std::min<std::uint64_t>(took / 1000, 65535));
            }
        }
    }
}

}  // namespace

std::uint64_t now_us() { return s_clock != nullptr ? s_clock() : static_cast<std::uint64_t>(lv_tick_get()) * 1000; }

void start(Clock clock)
{
    s_clock = clock;
    if (s_started) {
        return;
    }
    s_started = true;
    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, on_display, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(disp, on_display, LV_EVENT_REFR_READY, nullptr);
    s_second.from_us = now_us();
    lv_timer_create(
        [](lv_timer_t *) {
            const std::uint64_t t = now_us();
            s_last   = s_second.stats(t);
            s_second = Acc{};
            s_second.from_us = t;
        },
        1000, nullptr);
}

Stats last_second() { return s_last; }

int frames(const std::uint16_t **ms)
{
    *ms = s_each;
    return s_each_n;
}

void begin()
{
    s_phase         = Acc{};
    s_phase.from_us = now_us();
    s_phase_on      = true;
    s_each_n        = 0;
}

Stats end()
{
    s_phase_on = false;
    return s_phase.stats(now_us());
}

}  // namespace ui::kit::perf
