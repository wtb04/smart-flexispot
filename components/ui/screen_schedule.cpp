#include "ui_internal.h"

#include "room_model.h"
#include "screen_rules.h"
#include "status_model.h"
#include "topics.h"

#include <ctime>

// The screen going dark by itself, and lighting again, as screen_rules.h has
// it; looked at every second.
namespace ui::detail {
namespace {
constexpr time_t CLOCK_SET = 1'700'000'000;  // any earlier and the clock is not set yet

screen_rules::Schedule s_schedule;
std::int64_t           s_now     = 0;  // ms, as lv_tick_get() counts them but never wrapping
std::uint32_t          s_tick_at = 0;

bool night_now()
{
    const time_t now = wall_now();
    if (now < CLOCK_SET) {
        return false;
    }
    std::tm local{};
    localtime_r(&now, &local);
    return screen_rules::is_night(local.tm_hour * 60 + local.tm_min);
}

void look()
{
    const StatusState  &status = status_state();
    const LightsState  &lights = lights_state();
    screen_rules::Inputs in;
    in.screen_on  = status.screen_on;
    in.on_battery = status.on_battery;
    in.lit_known  = lights.room_known;
    in.lit        = lights.room_lit;
    in.phone      = status.present;
    in.night      = night_now();
    in.notice     = s_notice_card != nullptr && !lv_obj_is_hidden(s_notice_card);
    in.video_ms   = cinema_dark_after();
    in.untouched  = lv_display_get_inactive_time(nullptr);

    s_now += lv_tick_elaps(s_tick_at);
    s_tick_at = lv_tick_get();
    switch (s_schedule.step(in, s_now)) {
        case screen_rules::Action::Wake:
            ESP_LOGI(TAG, "screen lit: %s", in.lit ? "the light came on" : "the phone came back");
            set_screen_state(true);
            break;
        case screen_rules::Action::Dark:
            ESP_LOGI(TAG, "screen dark after %d s: %s", static_cast<int>(s_schedule.dark_after(in) / 1000),
                     in.video_ms != 0         ? "the film's own"
                     : in.on_battery          ? "on the battery"
                     : !s_schedule.phone_here() ? "the light off, the phone away"
                                                : "the light off, at night");
            set_screen_state(false);
            break;
        default:
            break;
    }
}
}  // namespace

void start_screen_schedule()
{
    s_tick_at = lv_tick_get();
    subscribe(Topic::Second, kNoView, look);
}

}  // namespace ui::detail
