// Host-side tests of which job runs when. No ESP-IDF needed: the core is
// driven here as the panel's workers drive it.
#include "job_core.h"

#include <gtest/gtest.h>
#include <cstdio>

using namespace jobs;

namespace {
constexpr std::int64_t MS = 1000;

Spec every(int period_ms, Lane lane = Lane::Quick)
{
    Spec spec;
    spec.lane      = lane;
    spec.period_ms = period_ms;
    return spec;
}

// Runs whatever is due on the lane at `now`, as done(); returns which ran.
int run_due(Core &core, Lane lane, std::int64_t now, Result result = done())
{
    Job job = kNoJob;
    if (!core.next(lane, now, job)) {
        return -1;
    }
    core.finish(job, result, 1, now);
    return job;
}

TEST(JobCore, period)
{
    Core core;
    core.conditions(true, true);
    const Job a = core.add(every(1000), 0);
    EXPECT_EQ(run_due(core, Lane::Quick, 0), a) << "runs at once when added";
    EXPECT_EQ(run_due(core, Lane::Quick, 999 * MS), -1) << "not again before its period";
    EXPECT_EQ(run_due(core, Lane::Quick, 1000 * MS), a) << "again after it";
    EXPECT_EQ(core.due(Lane::Quick, 1000 * MS), 2000 * MS) << "and is due a period on";
}

TEST(JobCore, first_and_poke)
{
    Core core;
    core.conditions(true, true);
    Spec later = every(0);
    later.first_ms = kNever;
    const Job a    = core.add(later, 0);
    EXPECT_EQ(run_due(core, Lane::Quick, 100000 * MS), -1) << "one only poked does not run by itself";
    core.poke(a, 5 * MS);
    EXPECT_EQ(run_due(core, Lane::Quick, 5 * MS), a) << "and runs when poked";
    EXPECT_EQ(run_due(core, Lane::Quick, 100000 * MS), -1) << "then waits to be poked again";
}

TEST(JobCore, poke_while_running)
{
    Core core;
    core.conditions(true, true);
    const Job a = core.add(every(60000), 0);
    Job       got = kNoJob;
    core.next(Lane::Quick, 0, got);
    core.poke(a, 1 * MS);
    core.finish(a, done(), 1, 2 * MS);
    EXPECT_EQ(run_due(core, Lane::Quick, 2 * MS), a) << "poked while running, it runs again straight after";
}

TEST(JobCore, one_at_a_time)
{
    Core core;
    core.conditions(true, true);
    core.add(every(10), 0);
    Job first = kNoJob, second = kNoJob;
    core.next(Lane::Quick, 0, first);
    EXPECT_TRUE(!core.next(Lane::Quick, 100 * MS, second)) << "a running job is not started twice";
}

TEST(JobCore, soonest_first)
{
    Core core;
    core.conditions(true, true);
    Spec late  = every(1000);
    late.first_ms = 50;
    Spec early = every(1000);
    early.first_ms = 20;
    core.add(late, 0);
    const Job e = core.add(early, 0);
    EXPECT_EQ(run_due(core, Lane::Quick, 100 * MS), e) << "the one due soonest goes first";
}

TEST(JobCore, lanes)
{
    Core core;
    core.conditions(true, true);
    const Job slow = core.add(every(1000, Lane::Slow), 0);
    EXPECT_EQ(run_due(core, Lane::Quick, 0), -1) << "a slow job is not on the quick lane";
    EXPECT_EQ(run_due(core, Lane::Slow, 0), slow) << "but on its own";
}

TEST(JobCore, waits_for_network_and_screen)
{
    Core core;
    core.conditions(false, true);
    Spec fetch   = every(1000, Lane::Slow);
    fetch.online = true;
    const Job f  = core.add(fetch, 0);
    EXPECT_EQ(run_due(core, Lane::Slow, 5000 * MS), -1) << "one needing the network waits for it";
    EXPECT_TRUE(core.status(f, 5000 * MS).waiting) << "and says so";
    core.conditions(true, true);
    EXPECT_EQ(run_due(core, Lane::Slow, 5000 * MS), f) << "and runs as soon as it comes";

    Spec shown  = every(1000);
    shown.lit   = true;
    const Job s = core.add(shown, 6000 * MS);
    core.conditions(true, false);
    EXPECT_EQ(run_due(core, Lane::Quick, 6000 * MS), -1) << "one needing the screen waits while it is dark";
    EXPECT_EQ(core.due(Lane::Quick, 6000 * MS), Core::kNeverUs) << "and does not keep the lane awake";
    core.conditions(true, true);
    EXPECT_EQ(run_due(core, Lane::Quick, 9000 * MS), s) << "and runs when it is lit";
}

TEST(JobCore, backoff)
{
    Core core;
    core.conditions(true, true);
    Spec feed         = every(600000, Lane::Slow);
    feed.retry_ms     = 1000;
    feed.max_retry_ms = 4000;
    const Job a       = core.add(feed, 0);
    run_due(core, Lane::Slow, 0, failed());
    EXPECT_EQ(core.due(Lane::Slow, 0), 1000 * MS) << "after a failure, its retry";
    run_due(core, Lane::Slow, 1000 * MS, failed());
    EXPECT_EQ(core.due(Lane::Slow, 1000 * MS), 3000 * MS) << "twice as long after the second";
    run_due(core, Lane::Slow, 3000 * MS, failed());
    run_due(core, Lane::Slow, 7000 * MS, failed());
    EXPECT_EQ(core.due(Lane::Slow, 7000 * MS), 11000 * MS) << "no longer than the most";
    EXPECT_EQ(core.status(a, 7000 * MS).failures, 4) << "counting each";
    run_due(core, Lane::Slow, 11000 * MS);
    EXPECT_EQ(core.due(Lane::Slow, 11000 * MS), 611000 * MS) << "and its period once it works";
    EXPECT_EQ(core.status(a, 11000 * MS).failures, 0) << "counted from none again";
}

TEST(JobCore, again_in_and_sleep)
{
    Core core;
    core.conditions(true, true);
    const Job a = core.add(every(1000), 0);
    run_due(core, Lane::Quick, 0, again_in(50));
    EXPECT_EQ(core.due(Lane::Quick, 0), 50 * MS) << "it can say when it wants to run next";
    run_due(core, Lane::Quick, 50 * MS, sleep());
    EXPECT_EQ(core.due(Lane::Quick, 50 * MS), Core::kNeverUs) << "or that it waits to be poked";
    core.poke(a, 60 * MS);
    EXPECT_EQ(run_due(core, Lane::Quick, 60 * MS), a) << "which wakes it";
}
}  // namespace

