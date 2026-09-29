// Host-side tests of what goes when, and what becomes of each request. No
// ESP-IDF needed: the core is driven here as the panel's workers drive it.
#include "net_core.h"

#include <cstdio>
#include <string>
#include <vector>

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

// Records what each request was told, by name.
struct Log {
    std::vector<std::string> said;

    Done done(const std::string &name)
    {
        return [this, name](const Response &response) {
            static const char *OUTCOMES[] = {"answered", "failed", "resting", "replaced", "expired", "cancelled"};
            said.push_back(name + " " + OUTCOMES[static_cast<int>(response.outcome)]);
        };
    }
    bool has(const std::string &line) const
    {
        for (const std::string &s : said) {
            if (s == line) {
                return true;
            }
        }
        return false;
    }
};

void run(Tells &tells)
{
    for (Tell &tell : tells) {
        for (Done &done : tell.done) {
            done(tell.response);
        }
    }
    tells.clear();
}

Request make(Host host, const char *path, Priority priority, Done done)
{
    Request request;
    request.host     = host;
    request.path     = path;
    request.priority = priority;
    request.done     = std::move(done);
    return request;
}

Exchange answer(int status = 200)
{
    Exchange exchange;
    exchange.status = status;
    exchange.body   = "{}";
    exchange.length = 2;
    return exchange;
}

Exchange failure(bool timed_out = false)
{
    Exchange exchange;
    exchange.error     = 1;
    exchange.timed_out = timed_out;
    return exchange;
}

// Takes the next and says which path it was, or "" for none.
std::string next_path(Core &core, std::int64_t now, Running &running, Tells &tells, bool screen_on = true)
{
    return core.next(now, true, screen_on, running, tells) ? running.request.path : "";
}

void test_order()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    core.submit(make(host, "background", Priority::Background, log.done("b")), 0, tells);
    core.submit(make(host, "now", Priority::Now, log.done("n")), 1, tells);
    core.submit(make(host, "tap", Priority::Tap, log.done("t")), 2, tells);
    check(next_path(core, 3, running, tells) == "tap", "a tap before everything");
    check(next_path(core, 3, running, tells) == "now", "then what is due");
    check(next_path(core, 3, running, tells) == "", "and no more than the host has connections for");
    core.finish(running, answer(), 4, tells);
    check(next_path(core, 4, running, tells) == "background", "background once one is free");
}

void test_background_held()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    core.submit(make(host, "one", Priority::Background, log.done("one")), 0, tells);
    core.submit(make(host, "two", Priority::Background, log.done("two")), 0, tells);
    check(next_path(core, 1, running, tells, false) == "", "no background while the screen is dark");
    check(next_path(core, 1, running, tells) == "one", "it goes once it is lit");
    check(next_path(core, 1, running, tells) == "", "one background request at a time");
}

void test_offline()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    core.submit(make(host, "tap", Priority::Tap, log.done("t")), 0, tells);
    check(!core.next(1, false, true, running, tells) && core.waiting() == 1, "nothing goes while offline");
    check(core.next(2, true, true, running, tells), "and it goes once online");
}

void test_replace()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    Request first = make(host, "first", Priority::Now, log.done("first"));
    first.key     = "feed";
    first.dedupe  = Dedupe::Replace;
    Request later = first;
    later.path    = "later";
    later.done    = log.done("later");
    core.submit(first, 0, tells);
    core.submit(later, 1, tells);
    run(tells);
    check(log.has("first replaced") && core.waiting() == 1, "a newer one takes the waiting one's place");
    check(next_path(core, 2, running, tells) == "later", "and is what goes");
}

void test_join()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    Request background = make(host, "details", Priority::Background, log.done("prefetch"));
    background.key     = "details:abc";
    background.dedupe  = Dedupe::Join;
    Request tap        = background;
    tap.priority       = Priority::Tap;
    tap.done           = log.done("tap");
    const Ticket a     = core.submit(background, 0, tells);
    const Ticket b     = core.submit(tap, 1, tells);
    check(a == b && core.waiting() == 1, "a second asking for the same joins the first");
    check(next_path(core, 2, running, tells, false) == "details", "at the more urgent priority, dark or not");
    core.finish(running, answer(), 3, tells);
    run(tells);
    check(log.has("prefetch answered") && log.has("tap answered"), "and both are told of the one answer");
}

void test_aging()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    const std::int64_t age = Core::kAgeUs;
    Request old            = make(host, "old", Priority::Background, log.done("old"));
    old.deadline_ms        = 60 * 1000;  // longer than it is allowed: capped, but past the test
    core.submit(old, 0, tells);
    core.submit(make(host, "fresh tap", Priority::Tap, log.done("tap")), 2 * age, tells);
    check(next_path(core, 2 * age, running, tells) == "old", "background that has waited long enough goes first");
}

void test_expired()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    Request tap     = make(host, "tap", Priority::Tap, log.done("tap"));
    tap.deadline_ms = 100;
    core.submit(tap, 0, tells);
    check(!core.next(200 * MS, false, true, running, tells), "offline");
    run(tells);
    check(log.has("tap expired") && core.waiting() == 0, "past its deadline it is told so");
}

void test_retry()
{
    Core       core;
    HostConfig config;
    config.retry = Retry{2, 100, 200, false};
    config.rest.failures = 10;
    Host    host         = core.add_host(config);
    Tells   tells;
    Log     log;
    Running running;
    core.submit(make(host, "flaky", Priority::Now, log.done("flaky")), 0, tells);
    next_path(core, 0, running, tells);
    core.finish(running, failure(), 0, tells);
    check(next_path(core, 99 * MS, running, tells) == "", "not tried again before its delay");
    check(next_path(core, 100 * MS, running, tells) == "flaky" && running.attempt == 2, "but after it");
    core.finish(running, failure(), 100 * MS, tells);
    check(next_path(core, 299 * MS, running, tells) == "", "the next delay twice as long");
    check(next_path(core, 300 * MS, running, tells) == "flaky" && running.attempt == 3, "and then again");
    core.finish(running, failure(), 300 * MS, tells);
    run(tells);
    check(log.has("flaky failed"), "failed once the attempts are spent");
}

void test_no_retry()
{
    Core       core;
    HostConfig config;
    config.retry = Retry{3, 0, 100, false};
    config.rest.failures = 10;
    Host    host         = core.add_host(config);
    Tells   tells;
    Log     log;
    Running running;
    core.submit(make(host, "slow", Priority::Now, log.done("slow")), 0, tells);
    next_path(core, 0, running, tells);
    core.finish(running, failure(true), 0, tells);
    run(tells);
    check(log.has("slow failed"), "no retry after a timeout, which the server may have acted on");

    Request post    = make(host, "post", Priority::Now, log.done("post"));
    post.repeatable = false;
    core.submit(post, 0, tells);
    next_path(core, 0, running, tells);
    core.finish(running, failure(), 0, tells);
    run(tells);
    check(log.has("post failed"), "nor for what is not safe to send twice");
}

void test_rest()
{
    Core       core;
    HostConfig config;
    config.rest = Rest{2, 1000, 500};
    Host    host = core.add_host(config);
    Tells   tells;
    Log     log;
    Running running;
    for (int i = 0; i < 2; ++i) {
        core.submit(make(host, "x", Priority::Now, log.done("x")), 0, tells);
        next_path(core, 0, running, tells);
        core.finish(running, failure(), 0, tells);
    }
    check(core.status(host, 0).resting, "resting after two failures in a row");

    Request hurried     = make(host, "hurried", Priority::Now, log.done("hurried"));
    hurried.deadline_ms = 500;
    Request patient     = make(host, "patient", Priority::Now, log.done("patient"));
    core.submit(hurried, 0, tells);
    core.submit(patient, 0, tells);
    check(next_path(core, 10 * MS, running, tells) == "", "nothing goes while it rests");
    run(tells);
    check(log.has("hurried resting"), "one that cannot wait is told at once, to go elsewhere");
    check(next_path(core, 1000 * MS, running, tells) == "patient", "one that can goes when the rest is over");
    core.finish(running, answer(), 1000 * MS, tells);
    check(core.status(host, 1000 * MS).failures_in_row == 0, "an answer ends the run of failures");
}

void test_busy()
{
    Core       core;
    HostConfig config;
    config.rest = Rest{2, 1000, 500};
    Host    host = core.add_host(config);
    Tells   tells;
    Log     log;
    Running running;
    core.submit(make(host, "a", Priority::Now, log.done("a")), 0, tells);
    next_path(core, 0, running, tells);
    Exchange busy       = answer(429);
    busy.retry_after_ms = 2000;
    core.finish(running, busy, 0, tells);
    run(tells);
    check(log.has("a answered"), "a 429 is an answer, for its asker to read");
    check(core.status(host, 1999 * MS).resting && !core.status(host, 2000 * MS).resting,
          "and rests the host as long as its Retry-After says");
}

void test_cancel()
{
    Core    core;
    Host    host = core.add_host(HostConfig{});
    Tells   tells;
    Log     log;
    Running running;
    const Ticket waiting = core.submit(make(host, "waiting", Priority::Now, log.done("waiting")), 0, tells);
    core.submit(make(host, "sent", Priority::Tap, log.done("sent")), 0, tells);
    next_path(core, 0, running, tells);
    core.cancel(waiting, tells);
    core.cancel(running.ticket, tells);
    run(tells);
    check(log.has("waiting cancelled") && !log.has("sent cancelled"), "a waiting one is told at once");
    core.finish(running, answer(), 1, tells);
    run(tells);
    check(log.has("sent cancelled") && !log.has("sent answered"), "one under way when its answer comes");
}
}  // namespace

int main()
{
    test_order();
    test_background_held();
    test_offline();
    test_replace();
    test_join();
    test_aging();
    test_expired();
    test_retry();
    test_no_retry();
    test_rest();
    test_busy();
    test_cancel();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
