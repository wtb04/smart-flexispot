#include "ui_internal.h"

#include "room_model.h"
#include "screen_rules.h"
#include "status_model.h"
#include "topics.h"

#include <ctime>

// The one place the screen goes dark by itself or lights again, as
// screen_rules.h has it. A touch, Home Assistant and Setup's Screen off still
// switch it themselves; this only follows what they did.
namespace ui::detail {
namespace {
constexpr time_t CLOCK_SET = 1'700'000'000;  // any earlier and the clock is not set yet

screen_rules::Schedule s_schedule;
std::int64_t           s_now     = 0;  // ms since the first look, as lv_tick_get() counts but never wrapping
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

screen_rules::Inputs inputs()
{
    const StatusState &status = status_state();
    const LightsState &lights = lights_state();
    screen_rules::Inputs in;
    in.screen_on  = status.screen_on;
    in.unplugged  = status.on_battery;
    in.at_desk    = desk_state().available;
    in.room_known = lights.room_known;
    in.room_lit   = lights.room_lit;
    in.phone      = status.present;
    in.night      = night_now();
    in.attention  = notice_on_show() || cinema_offers_skip();
    in.film_ms    = cinema_dark_after();
    in.untouched  = lv_display_get_inactive_time(nullptr);
    return in;
}

void look()
{
    s_now += lv_tick_elaps(s_tick_at);
    s_tick_at = lv_tick_get();
    const screen_rules::Decision decision = s_schedule.step(inputs(), s_now);
    if (decision.action == screen_rules::Action::Keep) {
        return;
    }
    const bool on = decision.action == screen_rules::Action::Wake;
    ESP_LOGI(TAG, "screen %s: %s", on ? "lit" : "dark", screen_rules::describe(decision.why));
    set_screen_state(on);
}
}  // namespace

void start_screen_schedule()
{
    s_tick_at = lv_tick_get();
    for (Topic topic : {Topic::Second, Topic::Notices, Topic::Lights, Topic::Status, Topic::Desk}) {
        subscribe(topic, kNoView, look);
    }
}

}  // namespace ui::detail
