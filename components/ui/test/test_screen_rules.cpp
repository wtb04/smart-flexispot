#include "screen_rules.h"

#include <gtest/gtest.h>

namespace {
using ui::screen_rules::Action;
using ui::screen_rules::Inputs;
using ui::screen_rules::Schedule;

constexpr std::int64_t S   = 1000;
constexpr std::int64_t MIN = 60 * S;

Inputs dark_room()
{
    Inputs in;
    in.lit_known = true;
    in.phone     = true;
    return in;
}

// Steps once a second from `from` to `to`, nobody touching, and says what the
// screen was told last; it follows what it is told.
Action run(Schedule &schedule, Inputs &in, std::int64_t from, std::int64_t to)
{
    Action last = Action::Keep;
    for (std::int64_t t = from; t <= to; t += S) {
        in.untouched = t;
        const Action action = schedule.step(in, t);
        if (action != Action::Keep) {
            last         = action;
            in.screen_on = action == Action::Wake;
        }
    }
    return last;
}
}  // namespace

TEST(ScreenRules, night)
{
    using ui::screen_rules::is_night;
    EXPECT_TRUE(!is_night(22 * 60 + 29) && is_night(22 * 60 + 30) && is_night(0) && is_night(6 * 60 + 59) &&
                !is_night(7 * 60))
        << "night from half past ten to seven";
}

TEST(ScreenRules, light_on_stays_lit)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.lit      = true;
    in.phone    = false;
    in.night    = true;
    EXPECT_EQ(run(schedule, in, 0, 30 * MIN), Action::Keep) << "the light on: never dark, phone or not";
}

TEST(ScreenRules, phone_by_day_stays_lit)
{
    Schedule schedule;
    Inputs   in = dark_room();
    EXPECT_EQ(run(schedule, in, 0, 30 * MIN), Action::Keep) << "the light off, the phone here, by day";
}

TEST(ScreenRules, phone_at_night_a_minute)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.night    = true;
    EXPECT_EQ(run(schedule, in, 0, 59 * S), Action::Keep);
    EXPECT_EQ(run(schedule, in, 60 * S, 60 * S), Action::Dark) << "a minute at night";
}

TEST(ScreenRules, phone_away_only_after_five_minutes)
{
    Schedule schedule;
    Inputs   in = dark_room();
    run(schedule, in, 0, 10 * S);
    in.phone = false;
    EXPECT_EQ(run(schedule, in, 11 * S, 10 * S + 5 * MIN - S), Action::Keep) << "a phone unheard a while is no fluke";
    EXPECT_EQ(run(schedule, in, 10 * S + 5 * MIN, 10 * S + 5 * MIN + 29 * S), Action::Keep)
        << "the half minute starts once it counts as away";
    EXPECT_EQ(run(schedule, in, 10 * S + 5 * MIN + 30 * S, 10 * S + 5 * MIN + 30 * S), Action::Dark);
}

TEST(ScreenRules, phone_flickering_never_counts_as_away)
{
    Schedule schedule;
    Inputs   in = dark_room();
    for (std::int64_t t = 0; t < 30 * MIN; t += 4 * MIN) {
        in.phone = false;
        EXPECT_EQ(run(schedule, in, t, t + 4 * MIN - S), Action::Keep);
        in.phone = true;
        run(schedule, in, t + 4 * MIN - S, t + 4 * MIN - S);
    }
}

TEST(ScreenRules, not_heard_since_the_start_is_given_its_time)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.phone    = false;
    EXPECT_EQ(run(schedule, in, 0, 5 * MIN - S), Action::Keep);
    EXPECT_EQ(run(schedule, in, 5 * MIN, 5 * MIN + 30 * S), Action::Dark);
}

TEST(ScreenRules, a_touch_starts_the_wait_again)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.night    = true;
    run(schedule, in, 0, 50 * S);
    in.untouched = 0;
    EXPECT_EQ(schedule.step(in, 51 * S), Action::Keep);
    in.untouched = 59 * S;
    EXPECT_EQ(schedule.step(in, 110 * S), Action::Keep) << "a minute from the touch, not from the start";
    in.untouched = 60 * S;
    EXPECT_EQ(schedule.step(in, 111 * S), Action::Dark) << "and dark all the same";
}

TEST(ScreenRules, the_light_going_off_starts_the_wait)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.lit      = true;
    in.phone    = false;
    run(schedule, in, 0, 10 * MIN);
    in.lit = false;
    EXPECT_EQ(run(schedule, in, 10 * MIN + S, 10 * MIN + 30 * S), Action::Keep)
        << "left alone ten minutes, it still waits the half minute";
    EXPECT_EQ(run(schedule, in, 10 * MIN + 31 * S, 10 * MIN + 31 * S), Action::Dark);
}

TEST(ScreenRules, morning_does_not_wake)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.night    = true;
    EXPECT_EQ(run(schedule, in, 0, 2 * MIN), Action::Dark);
    in.night = false;
    EXPECT_EQ(run(schedule, in, 2 * MIN + S, 3 * MIN), Action::Keep) << "the night ending lights nothing";
    EXPECT_FALSE(in.screen_on);
}

TEST(ScreenRules, the_light_coming_on_wakes)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.night    = true;
    run(schedule, in, 0, 2 * MIN);
    in.lit = true;
    EXPECT_EQ(schedule.step(in, 2 * MIN + S), Action::Wake);
}

TEST(ScreenRules, the_phone_coming_back_wakes)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.phone    = false;
    EXPECT_EQ(run(schedule, in, 0, 6 * MIN), Action::Dark);
    in.phone = true;
    EXPECT_EQ(schedule.step(in, 6 * MIN + S), Action::Wake);
}

TEST(ScreenRules, the_phone_back_from_a_fluke_does_not_wake)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.night    = true;
    EXPECT_EQ(run(schedule, in, 0, 2 * MIN), Action::Dark);
    in.phone = false;
    run(schedule, in, 2 * MIN + S, 3 * MIN);
    in.phone = true;
    EXPECT_EQ(run(schedule, in, 3 * MIN + S, 4 * MIN), Action::Keep) << "it never counted as away";
}

TEST(ScreenRules, unknown_lights_stay_lit)
{
    Schedule schedule;
    Inputs   in;
    in.phone = false;
    EXPECT_EQ(run(schedule, in, 0, 30 * MIN), Action::Keep) << "before Home Assistant has told the lights";
}

TEST(ScreenRules, on_battery_half_a_minute)
{
    Schedule schedule;
    Inputs   in   = dark_room();
    in.lit        = true;
    in.on_battery = true;
    EXPECT_EQ(run(schedule, in, 0, 29 * S), Action::Keep);
    EXPECT_EQ(run(schedule, in, 30 * S, 30 * S), Action::Dark) << "away from the desk, whatever its lights";
}

TEST(ScreenRules, a_notice_keeps_it_lit)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.phone    = false;
    in.notice   = true;
    EXPECT_EQ(run(schedule, in, 0, 30 * MIN), Action::Keep);
    in.notice = false;
    EXPECT_EQ(run(schedule, in, 30 * MIN + S, 30 * MIN + S), Action::Dark);
}

TEST(ScreenRules, the_film_has_its_own)
{
    Schedule schedule;
    Inputs   in = dark_room();
    in.lit      = true;
    in.video_ms = 15 * S;
    EXPECT_EQ(run(schedule, in, 0, 15 * S), Action::Dark) << "its fifteen seconds, the light on or not";

    Schedule kept;
    Inputs   lit = dark_room();
    lit.phone    = false;
    lit.video_ms = ui::screen_rules::kNever;
    EXPECT_EQ(run(kept, lit, 0, 30 * MIN), Action::Keep) << "its own switched off keeps it lit";
}
