#pragma once

#include "esp_err.h"
#include "net_types.h"

#include <string>

// Every request the panel makes, in one place. Each waits its turn by what it
// is for, can join or replace one already asked, goes out on a connection
// kept for its host, is tried again after a delay when it fails, waits while
// there is no network, and is not sent to a host resting after failures or
// being told it asks too much. A few go at once, so none waits on a slow
// host's. What a live connection carries stays with whoever keeps it.
//
//     net::HostConfig config;
//     config.name = "adsbdb";
//     config.base = "https://api.adsbdb.com";
//     const net::Host host = net::add_host(config);
//
//     net::Request request;
//     request.host     = host;
//     request.path     = "/v0/aircraft/4ca27a";
//     request.priority = net::Priority::Tap;
//     request.key      = "details";
//     request.dedupe   = net::Dedupe::Replace;
//     request.done     = [](const net::Response &answer) {
//         if (answer.ok()) { /* answer.body */ }
//     };
//     net::submit(std::move(request));
namespace net {

esp_err_t start();

/** Once for each host, before its first request. Thread-safe. */
Host add_host(const HostConfig &config);

/** Thread-safe; `done` is called once, on one of the workers, or at once on
 *  the caller's task for one it replaces. */
Ticket submit(Request request);

/** The host for a whole address, found by its origin, or added with `like`'s
 *  settings when it is new: for addresses that come at run time, as covers
 *  do. Thread-safe. */
Host host_for(const std::string &url, const HostConfig &like);

/** What fetch() came back with; the body is where it was asked to go. */
struct Fetched {
    Outcome     outcome   = Outcome::Failed;
    int         status    = 0;
    int         error     = 0;
    std::size_t length    = 0;
    int         ms        = 0;
    bool        truncated = false;  // the body was more than there was room for

    bool ok() const { return outcome == Outcome::Answered && status >= 200 && status < 300; }
};

/** For a task that simply waits for the answer: sends `request`, whose `done`
 *  is not used, and waits on the caller's task for what becomes of it, the
 *  body copied into `into`, NUL-terminated. Given kFetchDeadlineMs when it
 *  says none, so that it cannot wait for ever offline. Not from a callback. */
Fetched fetch(Request request, char *into, std::size_t size);
Fetched fetch(Request request, std::string &into);

inline constexpr int kFetchDeadlineMs = 60 * 1000;

void cancel(Ticket ticket);
void cancel(Host host, const std::string &key);

HostStatus status(Host host);
bool       resting(Host host);

/** Dark: background requests wait until it is lit. */
void set_screen(bool on);

}  // namespace net
