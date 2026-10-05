#include "screen_rules.h"

#include <gtest/gtest.h>

namespace {
using namespace ui::screen_rules;

constexpr std::int64_t S   = 1000;
constexpr std::int64_t MIN = 60 * S;

// The panel as it follows the schedule: looked at as things change and once a
// second, the screen doing what it is told, and a finger on it only when
// touch() says so.
struct Panel {
    Schedule     schedule;
    Inputs       in;
    std::int64_t now        = 0;
    std::int64_t touched_at = 0;

    Panel()
    {
        in.room_known = true;
        in.phone      = true;
    }

    Decision look()
    {
        in.untouched = now - touched_at;
        const Decision decision = schedule.step(in, now);
        if (decision.action != Action::Keep) {
            in.screen_on = decision.action == Action::Wake;
        }
        return decision;
    }

    /** Looks at once, as the panel does when something changes, then once a
     *  second for `ms`; gives the last thing it was told. */
    Decision wait(std::int64_t ms)
    {
        Decision last = look();
        for (const std::int64_t until = now + ms; now < until;) {
            now += S;
            if (const Decision decision = look(); decision.action != Action::Keep) {
                last = decision;
            }
        }
        return last;
    }

    void touch()
    {
        touched_at   = now;
        in.screen_on = true;
    }
};

Decision dark(Why why) { return {Action::Dark, why}; }
Decision wake(Why why) { return {Action::Wake, why}; }
constexpr Decision kept{};

}  // namespace

namespace ui::screen_rules {
std::ostream &operator<<(std::ostream &out, const Decision &d)
{
    const char *action = d.action == Action::Keep ? "keep" : d.action == Action::Wake ? "wake" : "dark";
    return out << action << " (" << describe(d.why) << ")";
}
}  // namespace ui::screen_rules

TEST(ScreenRules, night)
{
    EXPECT_TRUE(!is_night(22 * 60 + 29) && is_night(22 * 60 + 30) && is_night(0) && is_night(6 * 60 + 59) &&
                !is_night(7 * 60))
        << "night from half past ten to seven";
}

TEST(ScreenRules, the_light_on_keeps_it_lit)
{
    Panel panel;
    panel.in.room_lit = true;
    panel.in.phone    = false;
    panel.in.night    = true;
    EXPECT_EQ(panel.wait(30 * MIN), kept) << "phone or not, night or not";
}

TEST(ScreenRules, the_phone_by_day_keeps_it_lit)
{
    Panel panel;
    EXPECT_EQ(panel.wait(30 * MIN), kept);
}

TEST(ScreenRules, the_phone_at_night_a_minute)
{
    Panel panel;
    panel.in.night = true;
    EXPECT_EQ(panel.wait(59 * S), kept);
    EXPECT_EQ(panel.wait(S), dark(Why::Night));
}

TEST(ScreenRules, the_phone_gone_only_after_five_minutes)
{
    Panel panel;
    panel.wait(10 * S);
    panel.in.phone = false;
    EXPECT_EQ(panel.wait(5 * MIN - S), kept) << "a phone unheard a while is no fluke";
    EXPECT_EQ(panel.wait(30 * S), kept) << "the half minute starts once it counts as gone";
    EXPECT_EQ(panel.wait(S), dark(Why::Empty));
}

TEST(ScreenRules, a_flickering_phone_never_counts_as_gone)
{
    Panel panel;
    for (int i = 0; i < 8; ++i) {
        panel.in.phone = false;
        EXPECT_EQ(panel.wait(4 * MIN), kept);
        panel.in.phone = true;
        panel.wait(S);
    }
}

TEST(ScreenRules, a_phone_not_heard_since_the_start_gets_its_time)
{
    Panel panel;
    panel.in.phone = false;
    EXPECT_EQ(panel.wait(5 * MIN + 29 * S), kept);
    EXPECT_EQ(panel.wait(S), dark(Why::Empty));
}

TEST(ScreenRules, a_touch_starts_the_wait_again_and_dark_all_the_same)
{
    Panel panel;
    panel.in.night = true;
    panel.wait(50 * S);
    panel.touch();
    EXPECT_EQ(panel.wait(59 * S), kept) << "a minute from the touch, not from the start";
    EXPECT_EQ(panel.wait(S), dark(Why::Night));
}

TEST(ScreenRules, the_light_going_off_starts_the_wait)
{
    Panel panel;
    panel.in.room_lit = true;
    panel.in.phone    = false;
    panel.wait(10 * MIN);
    panel.in.room_lit = false;
    EXPECT_EQ(panel.wait(29 * S), kept) << "left alone ten minutes, it still waits the half minute";
    EXPECT_EQ(panel.wait(S), dark(Why::Empty));
}

TEST(ScreenRules, the_morning_does_not_wake_it)
{
    Panel panel;
    panel.in.night = true;
    EXPECT_EQ(panel.wait(2 * MIN), dark(Why::Night));
    panel.in.night = false;
    EXPECT_EQ(panel.wait(MIN), kept);
    EXPECT_FALSE(panel.in.screen_on);
}

TEST(ScreenRules, the_light_coming_on_wakes_it)
{
    Panel panel;
    panel.in.night = true;
    panel.wait(2 * MIN);
    panel.in.room_lit = true;
    EXPECT_EQ(panel.wait(S), wake(Why::LightOn));
    EXPECT_EQ(panel.wait(30 * MIN), kept);
}

TEST(ScreenRules, the_phone_coming_back_wakes_it)
{
    Panel panel;
    panel.in.phone = false;
    EXPECT_EQ(panel.wait(6 * MIN), dark(Why::Empty));
    panel.in.phone = true;
    EXPECT_EQ(panel.wait(S), wake(Why::PhoneBack));
}

TEST(ScreenRules, the_phone_back_from_a_fluke_does_not_wake_it)
{
    Panel panel;
    panel.in.night = true;
    EXPECT_EQ(panel.wait(2 * MIN), dark(Why::Night));
    panel.in.phone = false;
    panel.wait(MIN);
    panel.in.phone = true;
    EXPECT_EQ(panel.wait(MIN), kept) << "it never counted as gone";
}

TEST(ScreenRules, lights_not_told_yet_keep_it_lit)
{
    Panel panel;
    panel.in.room_known = false;
    panel.in.phone      = false;
    EXPECT_EQ(panel.wait(30 * MIN), kept);
}

TEST(ScreenRules, unplugged_away_from_the_desk_half_a_minute)
{
    Panel panel;
    panel.in.room_lit  = true;
    panel.in.unplugged = true;
    EXPECT_EQ(panel.wait(29 * S), kept);
    EXPECT_EQ(panel.wait(S), dark(Why::Away)) << "whatever its room's lights";
}

TEST(ScreenRules, unplugged_at_the_desk_as_plugged_in)
{
    Panel panel;
    panel.in.room_lit  = true;
    panel.in.unplugged = true;
    panel.in.at_desk   = true;
    EXPECT_EQ(panel.wait(30 * MIN), kept);
    panel.in.room_lit = false;
    panel.in.night    = true;
    EXPECT_EQ(panel.wait(MIN), dark(Why::Night));
}

TEST(ScreenRules, a_notice_wakes_it_and_it_goes_dark_after)
{
    Panel panel;
    panel.in.room_lit = true;
    panel.in.screen_on = false;  // switched off by hand
    panel.in.attention = true;
    EXPECT_EQ(panel.wait(S), wake(Why::Attention));
    EXPECT_EQ(panel.wait(20 * S), kept) << "lit for the notice's whole time";
    panel.in.attention = false;
    EXPECT_EQ(panel.wait(S), dark(Why::Attention)) << "lit for it alone, dark again after";
}

TEST(ScreenRules, a_notice_touched_stays_lit)
{
    Panel panel;
    panel.in.room_lit  = true;
    panel.in.screen_on = false;
    panel.in.attention = true;
    panel.wait(5 * S);
    panel.touch();
    panel.in.attention = false;
    EXPECT_EQ(panel.wait(30 * MIN), kept) << "touched: someone is there, the light's rule again";
}

TEST(ScreenRules, a_notice_holds_it_past_the_wait)
{
    Panel panel;
    panel.in.phone     = false;
    panel.in.attention = true;
    EXPECT_EQ(panel.wait(30 * MIN), kept);
    panel.in.attention = false;
    EXPECT_EQ(panel.wait(S), dark(Why::Empty)) << "left alone all along";
}

TEST(ScreenRules, the_film_has_its_own_wait)
{
    Panel panel;
    panel.in.room_lit = true;
    panel.in.film_ms  = 15 * S;
    EXPECT_EQ(panel.wait(14 * S), kept);
    EXPECT_EQ(panel.wait(S), dark(Why::Film)) << "the light on or not";

    Panel held;
    held.in.phone   = false;
    held.in.film_ms = kNever;
    EXPECT_EQ(held.wait(30 * MIN), kept) << "its own switched off keeps it lit";
}

TEST(ScreenRules, a_skip_button_wakes_the_film)
{
    Panel panel;
    panel.in.film_ms = 15 * S;
    EXPECT_EQ(panel.wait(15 * S), dark(Why::Film));
    panel.in.attention = true;
    EXPECT_EQ(panel.wait(S), wake(Why::Attention));
    EXPECT_EQ(panel.wait(MIN), kept) << "lit while it can be tapped";
}
