// Which worker is stuck: busy with one thing past its limit. Waiting for work,
// however long, is not being stuck.
#include "watchdog_core.h"

#include <gtest/gtest.h>

using namespace watchdog;

namespace {
constexpr std::int64_t MS = 1000;
}  // namespace

TEST(WatchdogCore, idle_is_never_stuck)
{
    Core    core;
    Overdue found;
    core.add("jobs", 10 * 1000);
    EXPECT_FALSE(core.overdue(3600 * 1000 * MS, found)) << "waiting for work an hour is fine";
}

TEST(WatchdogCore, busy_within_its_limit_is_fine)
{
    Core       core;
    Overdue    found;
    const Beat beat = core.add("jobs", 10 * 1000);
    core.busy(beat, "clock", 0);
    EXPECT_FALSE(core.overdue(10 * 1000 * MS, found)) << "at its limit, not yet past it";
    core.idle(beat);
    EXPECT_FALSE(core.overdue(60 * 1000 * MS, found)) << "done in time, and idle after";
}

TEST(WatchdogCore, busy_past_its_limit_is_stuck)
{
    Core       core;
    Overdue    found;
    const Beat beat = core.add("jobs", 10 * 1000);
    core.busy(beat, "battery", 5 * MS);
    ASSERT_TRUE(core.overdue(10'006 * MS, found)) << "past its limit";
    EXPECT_EQ(found.who, "jobs");
    EXPECT_EQ(found.what, "battery") << "says what it was busy with";
    EXPECT_EQ(found.for_ms, 10'001);
}

TEST(WatchdogCore, told_once_each_busy_spell)
{
    Core       core;
    Overdue    found;
    const Beat beat = core.add("net", 90 * 1000);
    core.busy(beat, "radar feed", 0);
    EXPECT_TRUE(core.overdue(91 * 1000 * MS, found));
    EXPECT_FALSE(core.overdue(92 * 1000 * MS, found)) << "not again for the same spell";
    core.busy(beat, "radar feed", 100 * 1000 * MS);
    EXPECT_TRUE(core.overdue(191 * 1000 * MS, found)) << "a fresh spell can be stuck again";
}

TEST(WatchdogCore, each_worker_has_its_own_limit)
{
    Core       core;
    Overdue    found;
    const Beat quick = core.add("jobs", 10 * 1000);
    const Beat slow  = core.add("jobs_slow", 180 * 1000);
    core.busy(quick, "clock", 0);
    core.busy(slow, "ical", 0);
    core.idle(quick);
    EXPECT_FALSE(core.overdue(60 * 1000 * MS, found)) << "a slow job a minute in is not stuck";
    EXPECT_TRUE(core.overdue(181 * 1000 * MS, found));
    EXPECT_EQ(found.who, "jobs_slow");
}

TEST(WatchdogCore, unknown_beats_are_ignored)
{
    Core    core;
    Overdue found;
    core.busy(kNoBeat, "nothing", 0);
    core.idle(7);
    EXPECT_FALSE(core.overdue(1'000'000 * MS, found));
}
