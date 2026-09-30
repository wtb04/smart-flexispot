#include "watchdog_core.h"

namespace watchdog {
namespace {
constexpr std::int64_t US_PER_MS = 1000;
}  // namespace

Beat Core::add(const char *who, int limit_ms)
{
    Worker worker;
    worker.who      = who != nullptr ? who : "";
    worker.limit_ms = limit_ms;
    workers_.push_back(std::move(worker));
    return static_cast<Beat>(workers_.size() - 1);
}

void Core::busy(Beat beat, const char *what, std::int64_t now)
{
    if (beat < 0 || beat >= static_cast<Beat>(workers_.size())) {
        return;
    }
    Worker &worker  = workers_[beat];
    worker.busy     = true;
    worker.told     = false;
    worker.what     = what != nullptr ? what : "";
    worker.since_us = now;
}

void Core::idle(Beat beat)
{
    if (beat >= 0 && beat < static_cast<Beat>(workers_.size())) {
        workers_[beat].busy = false;
    }
}

bool Core::overdue(std::int64_t now, Overdue &out)
{
    for (Worker &worker : workers_) {
        const std::int64_t for_us = now - worker.since_us;
        if (worker.busy && !worker.told && for_us > worker.limit_ms * US_PER_MS) {
            worker.told = true;
            out         = {worker.who, worker.what, static_cast<int>(for_us / US_PER_MS)};
            return true;
        }
    }
    return false;
}

}  // namespace watchdog
