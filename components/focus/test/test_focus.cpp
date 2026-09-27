#include "focus_plan.h"

#include <cstdio>

namespace {
int checks   = 0;
int failures = 0;

void check(bool ok, const char *what)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}
}  // namespace

int main()
{
    using namespace focus;
    const Plan plan{};
    const std::int32_t min = units::kMsPerMinute;

    State s = toggled(State{}, plan, 1000);
    check(s.phase == Phase::Work && s.round == 1 && s.running, "the button starts round one");
    check(s.ends_at == 1000 + 25 * min, "a focus part is twenty-five minutes");
    check(left_of(s, 1000 + 10 * min) == 15 * min, "left is read off the clock");

    State paused = toggled(s, plan, 1000 + 10 * min);
    check(!paused.running && paused.left == 15 * min, "pausing keeps what is left");
    check(left_of(paused, 1000 + 25 * min) == 15 * min, "time does not pass while paused");
    State resumed = toggled(paused, plan, 1000 + 25 * min);
    check(resumed.running && resumed.ends_at == 1000 + 40 * min, "carrying on ends later by the pause");

    State b = after(s, plan, 0);
    check(b.phase == Phase::Break && b.round == 1 && b.length == 5 * min, "focus is followed by a break");
    State w = after(b, plan, 0);
    check(w.phase == Phase::Work && w.round == 2, "a break is followed by the next round");

    State fourth = part(Phase::Work, 4, plan, 0);
    State longer = after(fourth, plan, 0);
    check(longer.phase == Phase::LongBreak && longer.length == 20 * min,
          "the fourth round is followed by the long break");
    const State again = after(longer, plan, 0);
    check(again.phase == Phase::Work && again.round == 1 && again.running,
          "after the long break the next set starts at round one");

    const State rest = finished(s, plan, 0);
    check(rest.phase == Phase::Break && waiting(rest) && rest.left == 5 * min,
          "a break waits for the button once focus runs out");
    const State next = finished(rest, plan, 7000);
    check(next.phase == Phase::Work && next.round == 2 && !next.running && waiting(next) &&
              next.left == 25 * min,
          "the next round waits for the button once a break runs out");
    const State go = toggled(next, plan, 9000);
    check(go.running && go.ends_at == 9000 + 25 * min, "the button starts the waiting round");
    check(finished(longer, plan, 0).round == 1 && waiting(finished(longer, plan, 0)),
          "after the long break the next set waits too");
    check(!waiting(paused), "a round paused part way is not waiting");

    // Kept across a restart: 10 minutes into round one, by the wall clock.
    const Saved kept = saved(s, 1000 + 10 * min, 5'000'000);
    check(kept.running && kept.ends_s == 5'000'000 + 15 * 60, "a running part keeps its end by the wall clock");
    const State back = restored(kept, plan, 200, 5'000'000 + 60);
    check(back.phase == Phase::Work && back.round == 1 && back.running &&
              back.ends_at == 200 + 14 * min && back.length == 25 * min,
          "back after a minute off, a minute less is left");
    const State over = restored(kept, plan, 200, 5'000'000 + 20 * 60);
    check(over.phase == Phase::Break && waiting(over), "a part that ran out while off gives way to the next, waiting");
    const State held = restored(saved(paused, 0, 5'000'000), plan, 200, 5'000'000 + 3600);
    check(held.phase == Phase::Work && !held.running && held.left == 15 * min, "a paused part comes back paused");
    const State unset = restored(saved(s, 1000 + 10 * min, 0), plan, 200, 5'000'000);
    check(!unset.running && unset.left == 15 * min, "without the wall clock a running part is held where it was");
    check(restored(saved(State{}, 0, 5'000'000), plan, 0, 5'000'000).phase == Phase::Idle, "idle stays idle");

    check(left_of(s, s.ends_at + 5000) == 0, "left never goes below zero");
    check(left_of(State{}, 12345) == 0, "nothing is left when idle");

    std::printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) {
        std::printf("ALL PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
