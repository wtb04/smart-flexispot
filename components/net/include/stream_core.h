#pragma once

#include <cstdint>
#include <string>

// What a live connection is doing and what to do about it next: connect once
// there is a network, begin again after a while when it drops or its other
// end refuses, give up on one that opens and never gets going, and keep it
// alive. Pure, as net_core is: it is told the time and what happened, and
// says what to do, so the host tests run it as the panel does. Not
// thread-safe; whoever drives it keeps it under a lock.
namespace net {

/** Begun again `first_ms` after a drop, each time after that
 *  `backoff_percent` as long, up to `max_ms`; from `first_ms` again once it
 *  has been ready. */
struct Reconnect {
    int first_ms        = 1000;
    int max_ms          = 60 * 1000;
    int backoff_percent = 200;
};

struct StreamPolicy {
    Reconnect reconnect{};
    int       ready_within_ms = 20 * 1000;  // from starting to connect until ready, or begun again
    int       keep_alive_ms   = 0;          // asked for this often while ready; 0 for never
};

enum class StreamState : std::uint8_t {
    Offline,     // no network: not tried
    Waiting,     // to connect again, when due
    Connecting,  // started, not yet open
    Open,        // open, its session not yet going: signed in, subscribed
    Ready,       // going
};

inline const char *stream_state_name(StreamState state)
{
    switch (state) {
        case StreamState::Offline: return "offline";
        case StreamState::Waiting: return "waiting";
        case StreamState::Connecting: return "connecting";
        case StreamState::Open: return "open";
        case StreamState::Ready: return "ready";
    }
    return "?";
}

enum class StreamAction : std::uint8_t { None, Start, Stop, KeepAlive };

struct StreamStatus {
    StreamState   state      = StreamState::Offline;
    int           for_ms     = 0;  // in that state
    int           next_in_ms = 0;  // until the next try, when waiting
    std::uint32_t readies    = 0;  // times it has got going
    std::uint32_t drops      = 0;  // times it went, or would not come, before or after
    std::string   last_error;
};

class StreamCore {
public:
    explicit StreamCore(const StreamPolicy &policy) : policy_(policy) {}

    void network(bool up, std::int64_t now);
    void opened(std::int64_t now);
    void closed(std::int64_t now, const char *why);
    void ready(std::int64_t now);
    /** Its session was refused: stopped, and begun again after `retry_ms`,
     *  or after the next back-off when 0. */
    void fail(std::int64_t now, const char *why, int retry_ms);
    /** Stopped and begun again at once. */
    void restart(std::int64_t now);

    /** What to do at `now`; asked again until None. */
    StreamAction next(std::int64_t now);
    /** When next() will next have something to do, if nothing happens before. */
    std::int64_t due(std::int64_t now) const;

    StreamState  state() const { return state_; }
    StreamStatus status(std::int64_t now) const;

private:
    void enter(StreamState state, std::int64_t now);
    void drop(std::int64_t now, const char *why, int retry_ms);

    StreamPolicy  policy_;
    StreamState   state_         = StreamState::Offline;
    std::int64_t  since_us_      = 0;
    std::int64_t  attempt_us_    = 0;  // when this connect began, for ready_within_ms
    std::int64_t  due_us_        = 0;  // the next try, when waiting
    std::int64_t  keep_alive_us_ = 0;
    std::int64_t  delay_ms_      = 0;  // the back-off after the next drop; 0 for first_ms
    bool          running_       = false;
    bool          stop_wanted_   = false;
    std::uint32_t readies_       = 0;
    std::uint32_t drops_         = 0;
    std::string   last_error_;
};

}  // namespace net
