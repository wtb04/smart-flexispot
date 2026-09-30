// Host-side tests of how a live connection is kept: when it connects, when it
// begins again, and when it is let go. No ESP-IDF needed.
#include "stream_core.h"

#include <gtest/gtest.h>
#include <cstdio>

using namespace net;

namespace {
constexpr std::int64_t MS = 1000;

StreamPolicy policy()
{
    StreamPolicy p;
    p.reconnect       = Reconnect{1000, 8000, 200};
    p.ready_within_ms = 20000;
    return p;
}

// Brought up and got going at `now`.
void go(StreamCore &core, std::int64_t now)
{
    core.network(true, now);
    core.next(now);
    core.opened(now);
    core.ready(now);
}

TEST(StreamCore, waits_for_network)
{
    StreamCore core(policy());
    EXPECT_EQ(core.next(0), StreamAction::None) << "nothing is tried without a network";
    core.network(true, 5 * MS);
    EXPECT_EQ(core.next(5 * MS), StreamAction::Start) << "it connects as soon as there is one";
    EXPECT_EQ(core.state(), StreamState::Connecting) << "and is connecting";
    core.opened(6 * MS);
    EXPECT_EQ(core.state(), StreamState::Open) << "open once the transport says so";
    core.ready(7 * MS);
    EXPECT_EQ(core.state(), StreamState::Ready) << "and ready once its owner says so";
    EXPECT_EQ(core.status(7 * MS).readies, 1) << "counted once";
}

TEST(StreamCore, backoff)
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.closed(0, "refused");
    EXPECT_EQ(core.next(999 * MS / 1000), StreamAction::None) << "not again before the first delay";
    EXPECT_EQ(core.next(1000 * MS), StreamAction::Start) << "again after a second";
    core.closed(1000 * MS, "refused");
    EXPECT_EQ(core.due(1000 * MS), 3000 * MS) << "then after two";
    core.next(3000 * MS);
    core.closed(3000 * MS, "refused");
    EXPECT_EQ(core.due(3000 * MS), 7000 * MS) << "then after four";
    core.next(7000 * MS);
    core.closed(7000 * MS, "refused");
    core.next(15000 * MS);
    core.closed(15000 * MS, "refused");
    EXPECT_EQ(core.due(15000 * MS), 23000 * MS) << "and no longer than the most";
    EXPECT_EQ(core.status(15000 * MS).drops, 5) << "each drop counted";
    EXPECT_EQ(core.status(15000 * MS).last_error, "refused") << "with why";
}

TEST(StreamCore, ready_resets_backoff)
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.closed(0, "refused");            // next after 1 s
    core.next(1000 * MS);
    core.closed(1000 * MS, "refused");    // after 2 s
    core.next(3000 * MS);
    core.closed(3000 * MS, "refused");    // after 4 s
    core.next(7000 * MS);
    core.opened(7000 * MS);
    core.ready(7000 * MS);
    core.closed(60000 * MS, "gone");
    EXPECT_EQ(core.due(60000 * MS), 61000 * MS) << "after being ready, back after the first delay";
}

TEST(StreamCore, offline_stops)
{
    StreamCore core(policy());
    go(core, 0);
    core.network(false, 10 * MS);
    EXPECT_EQ(core.next(10 * MS), StreamAction::Stop) << "losing the network stops it";
    EXPECT_EQ(core.state(), StreamState::Offline) << "and it waits for the network";
    core.closed(11 * MS, "stopped");
    EXPECT_EQ(core.status(11 * MS).drops, 0) << "its closing then is no drop";
    EXPECT_EQ(core.next(20 * MS), StreamAction::None) << "not tried while offline";
    core.network(true, 30 * MS);
    EXPECT_EQ(core.next(30 * MS), StreamAction::Start) << "at once when it is back";
}

TEST(StreamCore, offline_while_waiting)
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.closed(0, "refused");
    core.network(false, 100 * MS);
    EXPECT_EQ(core.next(100 * MS), StreamAction::None) << "not stopped when not running";
}

TEST(StreamCore, fail_with_delay)
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.opened(0);
    core.fail(1 * MS, "token rejected", 60000);
    EXPECT_EQ(core.next(1 * MS), StreamAction::Stop) << "a refused session is stopped";
    EXPECT_EQ(core.due(1 * MS), 60001 * MS) << "and begun again when it was asked";
    core.closed(2 * MS, "stopped");
    EXPECT_EQ(core.status(2 * MS).drops, 1) << "counted as one drop";
    EXPECT_EQ(core.status(2 * MS).last_error, "token rejected") << "with why";
}

TEST(StreamCore, not_ready_in_time)
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.opened(1 * MS);
    EXPECT_EQ(core.next(19999 * MS), StreamAction::None) << "given its time to get going";
    EXPECT_EQ(core.next(20000 * MS), StreamAction::Stop) << "then stopped";
    EXPECT_EQ(core.state(), StreamState::Waiting) << "and begun again later";
    EXPECT_EQ(core.status(20000 * MS).last_error, "not ready in time") << "saying why";
}

TEST(StreamCore, never_opens)
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    EXPECT_EQ(core.next(20000 * MS), StreamAction::Stop) << "a connect that never opens is stopped too";
    EXPECT_EQ(core.status(20000 * MS).last_error, "did not open in time") << "saying why";
}

TEST(StreamCore, keep_alive)
{
    StreamPolicy p   = policy();
    p.keep_alive_ms  = 30000;
    StreamCore core(p);
    go(core, 0);
    EXPECT_EQ(core.next(29999 * MS), StreamAction::None) << "no keep-alive before it is due";
    EXPECT_EQ(core.next(30000 * MS), StreamAction::KeepAlive) << "one when it is";
    EXPECT_EQ(core.next(30001 * MS), StreamAction::None) << "and only one";
    EXPECT_EQ(core.due(30001 * MS), 60000 * MS) << "the next a period on";
}

TEST(StreamCore, restart)
{
    StreamCore core(policy());
    go(core, 0);
    core.restart(5 * MS);
    EXPECT_EQ(core.next(5 * MS), StreamAction::Stop) << "a restart stops it";
    core.closed(5 * MS, "stopped");  // as stopping it says
    EXPECT_EQ(core.next(5 * MS), StreamAction::Start) << "and starts it again at once";
    EXPECT_EQ(core.status(5 * MS).drops, 0) << "which is no drop";
}

TEST(StreamCore, stale_close_ignored)
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.closed(0, "refused");
    core.closed(0, "refused");
    EXPECT_EQ(core.status(0).drops, 1) << "a second close while waiting is not another drop";
}
}  // namespace

