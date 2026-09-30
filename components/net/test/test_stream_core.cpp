// Host-side tests of how a live connection is kept: when it connects, when it
// begins again, and when it is let go. No ESP-IDF needed.
#include "stream_core.h"

#include <cstdio>

using namespace net;

namespace {
int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

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

void test_waits_for_network()
{
    StreamCore core(policy());
    check(core.next(0) == StreamAction::None, "nothing is tried without a network");
    core.network(true, 5 * MS);
    check(core.next(5 * MS) == StreamAction::Start, "it connects as soon as there is one");
    check(core.state() == StreamState::Connecting, "and is connecting");
    core.opened(6 * MS);
    check(core.state() == StreamState::Open, "open once the transport says so");
    core.ready(7 * MS);
    check(core.state() == StreamState::Ready, "and ready once its owner says so");
    check(core.status(7 * MS).readies == 1, "counted once");
}

void test_backoff()
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.closed(0, "refused");
    check(core.next(999 * MS / 1000) == StreamAction::None, "not again before the first delay");
    check(core.next(1000 * MS) == StreamAction::Start, "again after a second");
    core.closed(1000 * MS, "refused");
    check(core.due(1000 * MS) == 3000 * MS, "then after two");
    core.next(3000 * MS);
    core.closed(3000 * MS, "refused");
    check(core.due(3000 * MS) == 7000 * MS, "then after four");
    core.next(7000 * MS);
    core.closed(7000 * MS, "refused");
    core.next(15000 * MS);
    core.closed(15000 * MS, "refused");
    check(core.due(15000 * MS) == 23000 * MS, "and no longer than the most");
    check(core.status(15000 * MS).drops == 5, "each drop counted");
    check(core.status(15000 * MS).last_error == "refused", "with why");
}

void test_ready_resets_backoff()
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
    check(core.due(60000 * MS) == 61000 * MS, "after being ready, back after the first delay");
}

void test_offline_stops()
{
    StreamCore core(policy());
    go(core, 0);
    core.network(false, 10 * MS);
    check(core.next(10 * MS) == StreamAction::Stop, "losing the network stops it");
    check(core.state() == StreamState::Offline, "and it waits for the network");
    core.closed(11 * MS, "stopped");
    check(core.status(11 * MS).drops == 0, "its closing then is no drop");
    check(core.next(20 * MS) == StreamAction::None, "not tried while offline");
    core.network(true, 30 * MS);
    check(core.next(30 * MS) == StreamAction::Start, "at once when it is back");
}

void test_offline_while_waiting()
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.closed(0, "refused");
    core.network(false, 100 * MS);
    check(core.next(100 * MS) == StreamAction::None, "not stopped when not running");
}

void test_fail_with_delay()
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.opened(0);
    core.fail(1 * MS, "token rejected", 60000);
    check(core.next(1 * MS) == StreamAction::Stop, "a refused session is stopped");
    check(core.due(1 * MS) == 60001 * MS, "and begun again when it was asked");
    core.closed(2 * MS, "stopped");
    check(core.status(2 * MS).drops == 1, "counted as one drop");
    check(core.status(2 * MS).last_error == "token rejected", "with why");
}

void test_not_ready_in_time()
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.opened(1 * MS);
    check(core.next(19999 * MS) == StreamAction::None, "given its time to get going");
    check(core.next(20000 * MS) == StreamAction::Stop, "then stopped");
    check(core.state() == StreamState::Waiting, "and begun again later");
    check(core.status(20000 * MS).last_error == "not ready in time", "saying why");
}

void test_never_opens()
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    check(core.next(20000 * MS) == StreamAction::Stop, "a connect that never opens is stopped too");
    check(core.status(20000 * MS).last_error == "did not open in time", "saying why");
}

void test_keep_alive()
{
    StreamPolicy p   = policy();
    p.keep_alive_ms  = 30000;
    StreamCore core(p);
    go(core, 0);
    check(core.next(29999 * MS) == StreamAction::None, "no keep-alive before it is due");
    check(core.next(30000 * MS) == StreamAction::KeepAlive, "one when it is");
    check(core.next(30001 * MS) == StreamAction::None, "and only one");
    check(core.due(30001 * MS) == 60000 * MS, "the next a period on");
}

void test_restart()
{
    StreamCore core(policy());
    go(core, 0);
    core.restart(5 * MS);
    check(core.next(5 * MS) == StreamAction::Stop, "a restart stops it");
    core.closed(5 * MS, "stopped");  // as stopping it says
    check(core.next(5 * MS) == StreamAction::Start, "and starts it again at once");
    check(core.status(5 * MS).drops == 0, "which is no drop");
}

void test_stale_close_ignored()
{
    StreamCore core(policy());
    core.network(true, 0);
    core.next(0);
    core.closed(0, "refused");
    core.closed(0, "refused");
    check(core.status(0).drops == 1, "a second close while waiting is not another drop");
}
}  // namespace

int main()
{
    test_waits_for_network();
    test_backoff();
    test_ready_resets_backoff();
    test_offline_stops();
    test_offline_while_waiting();
    test_fail_with_delay();
    test_not_ready_in_time();
    test_never_opens();
    test_keep_alive();
    test_restart();
    test_stale_close_ignored();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
