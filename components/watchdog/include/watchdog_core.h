#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Which worker has been busy with one thing for longer than it may be. Each
// says when it takes something on and when it is done: waiting for work, as
// long as it likes, is not being stuck. Pure, as net's and jobs' cores are, so
// the host tests drive it; not thread-safe, whoever drives it keeps a lock.
namespace watchdog {

using Beat                    = int;
inline constexpr Beat kNoBeat = -1;

struct Overdue {
    std::string who;   // the worker
    std::string what;  // what it has been busy with
    int         for_ms = 0;
};

class Core {
public:
    Beat add(const char *who, int limit_ms);
    void busy(Beat beat, const char *what, std::int64_t now);
    void idle(Beat beat);

    /** The first worker past its limit at `now`, if any; told once for each
     *  time it is busy. */
    bool overdue(std::int64_t now, Overdue &out);

private:
    struct Worker {
        std::string  who;
        int          limit_ms = 0;
        bool         busy     = false;
        bool         told     = false;
        std::string  what;
        std::int64_t since_us = 0;
    };
    std::vector<Worker> workers_;
};

}  // namespace watchdog
