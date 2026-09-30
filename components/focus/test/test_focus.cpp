#include "focus_plan.h"

#include <gtest/gtest.h>

using namespace focus;

namespace {
constexpr std::int32_t min = units::kMsPerMinute;

// Round one started at 1000 ms, and paused ten minutes in: where most of the
// cases below begin.
class FocusPlan : public ::testing::Test {
protected:
    const Plan  plan{};
    const State started = toggled(State{}, plan, 1000);
    const State paused  = toggled(started, plan, 1000 + 10 * min);
};
}  // namespace

TEST_F(FocusPlan, the_button_starts_round_one)
{
    EXPECT_TRUE(started.phase == Phase::Work && started.round == 1 && started.running) << "the button starts round one";
    EXPECT_EQ(started.ends_at, 1000 + 25 * min) << "a focus part is twenty-five minutes";
    EXPECT_EQ(left_of(started, 1000 + 10 * min), 15 * min) << "left is read off the clock";
}

TEST_F(FocusPlan, pausing_and_carrying_on)
{
    EXPECT_TRUE(!paused.running && paused.left == 15 * min) << "pausing keeps what is left";
    EXPECT_EQ(left_of(paused, 1000 + 25 * min), 15 * min) << "time does not pass while paused";
    const State resumed = toggled(paused, plan, 1000 + 25 * min);
    EXPECT_TRUE(resumed.running && resumed.ends_at == 1000 + 40 * min) << "carrying on ends later by the pause";
    EXPECT_FALSE(waiting(paused)) << "a round paused part way is not waiting";
}

TEST_F(FocusPlan, parts_follow_each_other)
{
    const State b = after(started, plan, 0);
    EXPECT_TRUE(b.phase == Phase::Break && b.round == 1 && b.length == 5 * min) << "focus is followed by a break";
    const State w = after(b, plan, 0);
    EXPECT_TRUE(w.phase == Phase::Work && w.round == 2) << "a break is followed by the next round";

    const State longer = after(part(Phase::Work, 4, plan, 0), plan, 0);
    EXPECT_TRUE(longer.phase == Phase::LongBreak && longer.length == 20 * min)
        << "the fourth round is followed by the long break";
    const State again = after(longer, plan, 0);
    EXPECT_TRUE(again.phase == Phase::Work && again.round == 1 && again.running)
        << "after the long break the next set starts at round one";
}

TEST_F(FocusPlan, a_part_that_runs_out_waits_for_the_button)
{
    const State rest = finished(started, plan, 0);
    EXPECT_TRUE(rest.phase == Phase::Break && waiting(rest) && rest.left == 5 * min)
        << "a break waits for the button once focus runs out";
    const State next = finished(rest, plan, 7000);
    EXPECT_TRUE(next.phase == Phase::Work && next.round == 2 && !next.running && waiting(next) &&
                next.left == 25 * min)
        << "the next round waits for the button once a break runs out";
    const State go = toggled(next, plan, 9000);
    EXPECT_TRUE(go.running && go.ends_at == 9000 + 25 * min) << "the button starts the waiting round";

    const State longer = after(part(Phase::Work, 4, plan, 0), plan, 0);
    EXPECT_TRUE(finished(longer, plan, 0).round == 1 && waiting(finished(longer, plan, 0)))
        << "after the long break the next set waits too";
}

TEST_F(FocusPlan, kept_across_a_restart)
{
    // Ten minutes into round one, by the wall clock.
    const Saved kept = saved(started, 1000 + 10 * min, 5'000'000);
    EXPECT_TRUE(kept.running && kept.ends_s == 5'000'000 + 15 * 60) << "a running part keeps its end by the wall clock";
    const State back = restored(kept, plan, 200, 5'000'000 + 60);
    EXPECT_TRUE(back.phase == Phase::Work && back.round == 1 && back.running && back.ends_at == 200 + 14 * min &&
                back.length == 25 * min)
        << "back after a minute off, a minute less is left";
    const State over = restored(kept, plan, 200, 5'000'000 + 20 * 60);
    EXPECT_TRUE(over.phase == Phase::Break && waiting(over)) << "a part that ran out while off gives way to the next, waiting";
    const State held = restored(saved(paused, 0, 5'000'000), plan, 200, 5'000'000 + 3600);
    EXPECT_TRUE(held.phase == Phase::Work && !held.running && held.left == 15 * min) << "a paused part comes back paused";
    const State unset = restored(saved(started, 1000 + 10 * min, 0), plan, 200, 5'000'000);
    EXPECT_TRUE(!unset.running && unset.left == 15 * min) << "without the wall clock a running part is held where it was";
    EXPECT_EQ(restored(saved(State{}, 0, 5'000'000), plan, 0, 5'000'000).phase, Phase::Idle) << "idle stays idle";
}

TEST_F(FocusPlan, left_never_goes_below_zero)
{
    EXPECT_EQ(left_of(started, started.ends_at + 5000), 0) << "left never goes below zero";
    EXPECT_EQ(left_of(State{}, 12345), 0) << "nothing is left when idle";
}
