#pragma once

#include "net_types.h"

#include <cstdint>
#include <string>
#include <vector>

// Which request goes when, and what becomes of each: priorities with aging,
// deduplication, retries after a delay, rests, deadlines and cancelling. Pure:
// it is given the time and what came back, and says what to send and whom to
// tell, so the host tests run it as the panel does and the simulator can too.
// Not thread-safe; whoever drives it keeps it under a lock.
namespace net {

/** What happened on the wire, from the transport. */
struct Exchange {
    int         status         = 0;   // 0 when nothing came back
    int         error          = 0;   // the transport's, 0 when answered
    bool        timed_out      = false;
    int         ms             = 0;
    int         retry_after_ms = 0;   // from a Retry-After header, 0 for none
    const char *body           = "";
    std::size_t length         = 0;
};

/** A request let go to be sent, on connection `slot` of its host. */
struct Running {
    Ticket  ticket  = kNoTicket;
    Host    host    = kNoHost;
    int     slot    = -1;
    int     attempt = 0;
    Request request;  // without its callbacks, which the core keeps
};

/** Callbacks to make, outside the lock, with what to tell them. */
struct Tell {
    std::vector<Done> done;
    Response          response;
};
using Tells = std::vector<Tell>;

class Core {
public:
    static constexpr std::int64_t kAgeUs = 10 * 1000 * 1000;

    Host              add_host(const HostConfig &config);
    const HostConfig &config(Host host) const { return hosts_[host].config; }
    int               host_count() const { return static_cast<int>(hosts_.size()); }

    /** Queues `request`, or joins or replaces one with its key. Returns its
     *  ticket, which is the joined one's when it joined. */
    Ticket submit(Request request, std::int64_t now, Tells &tells);

    /** The next to send at `now`, if one may go: none while offline, and
     *  background only while the screen is lit and none of it is under way.
     *  Those past their deadline, or resting past it, are told so. */
    bool next(std::int64_t now, bool online, bool screen_on, Running &out, Tells &tells);

    /** What came of `running`: those waiting on it are told, or it waits to
     *  be tried again. */
    void finish(const Running &running, const Exchange &exchange, std::int64_t now, Tells &tells);

    /** Told Cancelled at once when waiting, and when its answer comes if under way. */
    void cancel(Ticket ticket, Tells &tells);
    void cancel(Host host, const std::string &key, Tells &tells);

    HostStatus  status(Host host, std::int64_t now) const;
    std::size_t waiting() const;

private:
    struct Job {
        Ticket            ticket = kNoTicket;
        Request           request;
        std::vector<Done> done;
        int               attempt     = 0;
        std::int64_t      queued_us   = 0;
        std::int64_t      ready_us    = 0;  // not before, as after a failure
        std::int64_t      deadline_us = 0;
        bool              running     = false;
        bool              cancelled   = false;
    };
    struct HostState {
        HostConfig        config;
        std::vector<bool> busy;
        std::int64_t      rest_until      = 0;
        int               failures_in_row = 0;
        int               last_ms         = 0;
        std::uint32_t     requests        = 0;
        std::uint32_t     failures        = 0;
    };

    Job *find(Ticket ticket);
    void tell(Job &job, Outcome outcome, Tells &tells, int error = 0);
    void drop(Ticket ticket);
    bool background_running() const;

    std::vector<HostState> hosts_;
    std::vector<Job>       jobs_;
    Ticket                 next_ticket_ = 1;
};

}  // namespace net
