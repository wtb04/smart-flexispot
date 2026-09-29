#include "net_core.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace net {
namespace {
constexpr std::int64_t US_PER_MS   = 1000;
constexpr int          TOO_MANY    = 429;
constexpr int          UNAVAILABLE = 503;

bool busy_status(int status)
{
    return status == TOO_MANY || status == UNAVAILABLE;
}
}  // namespace

Host Core::add_host(const HostConfig &config)
{
    HostState state;
    state.config = config;
    state.busy.assign(static_cast<std::size_t>(std::max(config.connections, 1)), false);
    hosts_.push_back(std::move(state));
    return static_cast<Host>(hosts_.size() - 1);
}

Core::Job *Core::find(Ticket ticket)
{
    for (Job &job : jobs_) {
        if (job.ticket == ticket) {
            return &job;
        }
    }
    return nullptr;
}

void Core::tell(Job &job, Outcome outcome, Tells &tells, int error)
{
    Tell told;
    told.done              = std::move(job.done);
    told.response.outcome  = outcome;
    told.response.error    = error;
    told.response.attempts = job.attempt;
    tells.push_back(std::move(told));
}

void Core::drop(Ticket ticket)
{
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [ticket](const Job &job) { return job.ticket == ticket; }),
                jobs_.end());
}

bool Core::background_running() const
{
    return std::any_of(jobs_.begin(), jobs_.end(),
                       [](const Job &job) { return job.running && job.request.priority == Priority::Background; });
}

Ticket Core::submit(Request request, std::int64_t now, Tells &tells)
{
    if (request.host < 0 || request.host >= host_count()) {
        Job bad;
        if (request.done) {
            bad.done.push_back(std::move(request.done));
        }
        tell(bad, Outcome::Failed, tells);
        return kNoTicket;
    }
    if (request.priority == Priority::Background &&
        (request.deadline_ms <= 0 || request.deadline_ms > kBackgroundDeadlineMs)) {
        request.deadline_ms = kBackgroundDeadlineMs;
    }
    if (!request.retry) {
        request.retry = hosts_[request.host].config.retry;
    }
    const std::int64_t deadline = request.deadline_ms > 0 ? now + request.deadline_ms * US_PER_MS : 0;

    if (!request.key.empty() && request.dedupe != Dedupe::None) {
        for (Job &job : jobs_) {
            if (job.cancelled || job.request.host != request.host || job.request.key != request.key) {
                continue;
            }
            if (request.dedupe == Dedupe::Join) {
                if (request.done) {
                    job.done.push_back(std::move(request.done));
                }
                if (!job.running && request.priority < job.request.priority) {
                    job.request.priority = request.priority;
                }
                // Waits as long as the more patient of the two would.
                job.deadline_us = job.deadline_us == 0 || deadline == 0 ? 0 : std::max(job.deadline_us, deadline);
                return job.ticket;
            }
            if (!job.running) {  // Replace
                tell(job, Outcome::Replaced, tells);
                drop(job.ticket);
                break;
            }
        }
    }

    Job job;
    job.ticket      = next_ticket_++;
    job.queued_us   = now;
    job.deadline_us = deadline;
    if (request.done) {
        job.done.push_back(std::move(request.done));
    }
    job.request = std::move(request);
    if (next_ticket_ == kNoTicket) {
        next_ticket_ = 1;
    }
    jobs_.push_back(std::move(job));
    return jobs_.back().ticket;
}

bool Core::next(std::int64_t now, bool online, bool screen_on, Running &out, Tells &tells)
{
    // Those that can no longer go, told.
    std::vector<Ticket> gone;
    for (Job &job : jobs_) {
        if (job.running) {
            continue;
        }
        const HostState &host = hosts_[job.request.host];
        if (job.deadline_us != 0 && now >= job.deadline_us) {
            tell(job, Outcome::Expired, tells);
            gone.push_back(job.ticket);
        } else if (job.deadline_us != 0 && host.rest_until > now && host.rest_until >= job.deadline_us) {
            tell(job, Outcome::Resting, tells);
            gone.push_back(job.ticket);
        }
    }
    for (const Ticket ticket : gone) {
        drop(ticket);
    }
    if (!online) {
        return false;
    }

    const bool background = screen_on && !background_running();
    const auto urgency    = [now](const Job &job) {
        return static_cast<std::int64_t>(job.request.priority) - (now - job.queued_us) / kAgeUs;
    };
    Job *best      = nullptr;
    int  best_slot = -1;
    for (Job &job : jobs_) {
        if (job.running || job.ready_us > now || (job.request.priority == Priority::Background && !background)) {
            continue;
        }
        const HostState &host = hosts_[job.request.host];
        if (host.rest_until > now) {
            continue;
        }
        const auto free = std::find(host.busy.begin(), host.busy.end(), false);
        if (free == host.busy.end()) {
            continue;
        }
        if (best == nullptr || urgency(job) < urgency(*best) ||
            (urgency(job) == urgency(*best) && job.queued_us < best->queued_us)) {
            best      = &job;
            best_slot = static_cast<int>(free - host.busy.begin());
        }
    }
    if (best == nullptr) {
        return false;
    }
    HostState &host                              = hosts_[best->request.host];
    host.busy[static_cast<std::size_t>(best_slot)] = true;
    ++host.requests;
    best->running = true;
    ++best->attempt;
    out.ticket  = best->ticket;
    out.host    = best->request.host;
    out.slot    = best_slot;
    out.attempt = best->attempt;
    out.request = best->request;
    out.request.done = nullptr;
    return true;
}

void Core::finish(const Running &running, const Exchange &exchange, std::int64_t now, Tells &tells)
{
    HostState &host = hosts_[running.host];
    if (running.slot >= 0 && running.slot < static_cast<int>(host.busy.size())) {
        host.busy[static_cast<std::size_t>(running.slot)] = false;
    }
    host.last_ms = exchange.ms;
    Job *job     = find(running.ticket);
    if (job == nullptr) {
        return;
    }

    const bool answered = exchange.error == 0 && exchange.status > 0;
    if (answered) {
        if (busy_status(exchange.status)) {
            const int ms    = exchange.retry_after_ms > 0 ? exchange.retry_after_ms : host.config.rest.busy_ms;
            host.rest_until = std::max(host.rest_until, now + ms * US_PER_MS);
        } else {
            host.failures_in_row = 0;
        }
    } else {
        ++host.failures;
        ++host.failures_in_row;
        if (host.config.rest.failure_ms > 0 && host.failures_in_row >= std::max(host.config.rest.failures, 1)) {
            host.rest_until = std::max(host.rest_until, now + host.config.rest.failure_ms * US_PER_MS);
        }
    }

    if (job->cancelled) {
        tell(*job, Outcome::Cancelled, tells);
        drop(job->ticket);
        return;
    }
    if (!answered) {
        const Retry &retry = *job->request.retry;
        if (job->attempt <= retry.attempts && job->request.repeatable && (!exchange.timed_out || retry.after_timeout)) {
            std::int64_t delay = retry.delay_ms;
            for (int i = 1; i < job->attempt; ++i) {
                delay = delay * retry.backoff_percent / 100;
            }
            job->running  = false;
            job->ready_us = now + delay * US_PER_MS;
            return;
        }
        tell(*job, Outcome::Failed, tells, exchange.error);
        tells.back().response.ms = exchange.ms;
        drop(job->ticket);
        return;
    }
    tell(*job, Outcome::Answered, tells);
    Response &response = tells.back().response;
    response.status    = exchange.status;
    response.body      = exchange.body;
    response.length    = exchange.length;
    response.ms        = exchange.ms;
    drop(job->ticket);
}

void Core::cancel(Ticket ticket, Tells &tells)
{
    Job *job = find(ticket);
    if (job == nullptr) {
        return;
    }
    if (job->running) {
        job->cancelled = true;
        return;
    }
    tell(*job, Outcome::Cancelled, tells);
    drop(ticket);
}

void Core::cancel(Host host, const std::string &key, Tells &tells)
{
    std::vector<Ticket> matching;
    for (const Job &job : jobs_) {
        if (job.request.host == host && job.request.key == key) {
            matching.push_back(job.ticket);
        }
    }
    for (const Ticket ticket : matching) {
        cancel(ticket, tells);
    }
}

HostStatus Core::status(Host host, std::int64_t now) const
{
    HostStatus out;
    if (host < 0 || host >= host_count()) {
        return out;
    }
    const HostState &state = hosts_[host];
    out.resting            = state.rest_until > now;
    out.rest_left_ms       = out.resting ? static_cast<int>((state.rest_until - now) / US_PER_MS) : 0;
    out.failures_in_row    = state.failures_in_row;
    out.last_ms            = state.last_ms;
    out.requests           = state.requests;
    out.failures           = state.failures;
    return out;
}

std::size_t Core::waiting() const
{
    return static_cast<std::size_t>(
        std::count_if(jobs_.begin(), jobs_.end(), [](const Job &job) { return !job.running; }));
}

}  // namespace net
