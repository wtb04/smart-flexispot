#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

// What a component asks of net, and what it is told back. Free of ESP-IDF, so
// the core that decides what goes when, and its tests, and the simulator, use
// the same.
namespace net {

using Host                    = int;
inline constexpr Host kNoHost = -1;

using Ticket                      = std::uint32_t;
inline constexpr Ticket kNoTicket = 0;

/** What a request is for, most urgent first. A request counts as a class more
 *  urgent for every ten seconds it has waited, so none waits for ever. */
enum class Priority : std::uint8_t {
    Tap,         // somebody is looking at the screen waiting for it
    Now,         // due, as a feed read on the minute
    Background,  // wanted before it is asked for: one at a time, and not while the screen is dark
};

/** Tried again after a failure: `attempts` more times, the first after
 *  `delay_ms`, each after that `backoff_percent` as long again. Not after a
 *  timeout unless asked, as the server may have acted on it. */
struct Retry {
    int  attempts        = 0;
    int  delay_ms        = 500;
    int  backoff_percent = 200;
    bool after_timeout   = false;
};

/** How a host is spared: after `failures` in a row it rests `failure_ms`, and
 *  after a 429 or 503 `busy_ms`, or as long as its Retry-After says. A
 *  request that can wait past a rest waits; one whose deadline comes first
 *  is told at once, to go to another host if it has one. */
struct Rest {
    int failures   = 2;
    int failure_ms = 30 * 1000;
    int busy_ms    = 30 * 1000;
};

struct HostConfig {
    const char *name        = "";               // in the log
    const char *base        = "";               // "https://api.adsbdb.com", no trailing slash
    const char *agent       = "smart-flexispot";
    const char *headers     = "";               // sent with every request, "Name: value" a line
    int         timeout_ms  = 5 * 1000;
    int         connections = 2;                // at once, at most
    int         idle_ms     = 30 * 1000;        // a connection unused this long is closed
    bool        gzip        = false;            // asked for; whatever comes compressed is inflated
    bool        keep_open   = true;             // false: closed after each request
    Retry       retry{};                        // for requests that do not say
    Rest        rest{};
};

/** What becomes of a request with the same key, on the same host, as one
 *  already there. */
enum class Dedupe : std::uint8_t {
    None,     // both go
    Replace,  // the newer takes the place of one still waiting, which is told Replaced
    Join,     // one waiting or under way answers this one too, at the more urgent of the two
};

enum class Method : std::uint8_t { Get, Post };

enum class Outcome : std::uint8_t {
    Answered,   // an answer came, whatever its status
    Failed,     // none did, after every attempt
    Resting,    // the host rests past this one's deadline
    Replaced,   // a newer one with its key took its place
    Expired,    // its deadline came before it could go
    Cancelled,  // cancel() asked
};

inline const char *outcome_name(Outcome outcome)
{
    switch (outcome) {
        case Outcome::Answered: return "answered";
        case Outcome::Failed: return "failed";
        case Outcome::Resting: return "host resting";
        case Outcome::Replaced: return "replaced";
        case Outcome::Expired: return "expired";
        case Outcome::Cancelled: return "cancelled";
    }
    return "?";
}

struct Response {
    Outcome     outcome  = Outcome::Failed;
    int         status   = 0;       // HTTP, when answered
    int         error    = 0;       // the transport's, when it failed: an esp_err_t on the panel
    const char *body     = "";      // inflated, NUL-terminated, and valid only in the callback
    std::size_t length   = 0;
    int         ms       = 0;       // on the wire, the last attempt
    int         attempts = 0;

    bool ok() const { return outcome == Outcome::Answered && status >= 200 && status < 300; }
};

using Done = std::function<void(const Response &)>;

/** What background is given at most, so that while the screen is dark or the
 *  urgent keep coming it cannot pile up. */
inline constexpr int kBackgroundDeadlineMs = 30 * 1000;

struct Request {
    Host                 host     = kNoHost;
    std::string          path;                  // after the host's base, or a whole url on its host
    Method               method   = Method::Get;
    std::string          body;                  // posted as JSON
    Priority             priority = Priority::Now;
    std::string          key;                   // for Dedupe; empty for none
    Dedupe               dedupe   = Dedupe::None;
    int                  deadline_ms = 0;       // given up if not started within this; 0 for none
    std::optional<Retry> retry;                 // the host's when not given
    bool                 repeatable = true;     // safe to send twice: a retry, or once more on a fresh connection
    std::size_t          max_body = 64 * 1024;
    const char          *what     = "";         // in the log
    Done                 done;                  // always called, once, on a worker
};

/** How a host is doing, for diagnostics and for choosing another. */
struct HostStatus {
    bool          resting         = false;
    int           rest_left_ms    = 0;
    int           failures_in_row = 0;
    int           last_ms         = 0;
    std::uint32_t requests        = 0;
    std::uint32_t failures        = 0;
};

}  // namespace net
