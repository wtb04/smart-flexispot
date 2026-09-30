#include "job_core.h"

#include <algorithm>
#include <utility>

namespace jobs {
namespace {
constexpr std::int64_t US_PER_MS = 1000;

std::int64_t after(std::int64_t now, int ms)
{
    return ms < 0 ? Core::kNeverUs : now + static_cast<std::int64_t>(ms) * US_PER_MS;
}
}  // namespace

Job Core::add(Spec spec, std::int64_t now)
{
    Slot slot;
    slot.due_us = spec.first_ms == kNever ? kNeverUs : after(now, spec.first_ms);
    slot.spec   = std::move(spec);
    jobs_.push_back(std::move(slot));
    return static_cast<Job>(jobs_.size() - 1);
}

void Core::poke(Job job, std::int64_t now)
{
    if (job < 0 || job >= count()) {
        return;
    }
    Slot &slot = jobs_[job];
    if (slot.running) {
        slot.poked = true;
    } else {
        slot.due_us = std::min(slot.due_us, now);
    }
}

void Core::conditions(bool online, bool lit)
{
    online_ = online;
    lit_    = lit;
}

bool Core::can_run(const Slot &slot) const
{
    return !slot.running && (!slot.spec.online || online_) && (!slot.spec.lit || lit_);
}

bool Core::next(Lane lane, std::int64_t now, Job &out)
{
    int best = -1;
    for (int i = 0; i < count(); ++i) {
        const Slot &slot = jobs_[i];
        if (slot.spec.lane != lane || !can_run(slot) || slot.due_us > now) {
            continue;
        }
        if (best < 0 || slot.due_us < jobs_[best].due_us) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    jobs_[best].running = true;
    out                 = best;
    return true;
}

void Core::finish(Job job, const Result &result, int took_ms, std::int64_t now)
{
    if (job < 0 || job >= count()) {
        return;
    }
    Slot &slot   = jobs_[job];
    slot.running = false;
    ++slot.runs;
    slot.last_ms = took_ms;
    slot.most_ms = std::max(slot.most_ms, took_ms);

    int wait_ms = result.next_ms;
    if (result.ok) {
        slot.failures = 0;
        if (wait_ms == -1) {
            wait_ms = slot.spec.period_ms > 0 ? slot.spec.period_ms : kNever;
        }
    } else {
        ++slot.failures;
        if (wait_ms == -1) {
            std::int64_t backoff = slot.spec.retry_ms;
            for (int i = 1; i < slot.failures && backoff < slot.spec.max_retry_ms; ++i) {
                backoff *= 2;
            }
            wait_ms = static_cast<int>(std::min<std::int64_t>(backoff, slot.spec.max_retry_ms));
        }
    }
    slot.due_us = wait_ms == kNever ? kNeverUs : after(now, wait_ms);
    if (std::exchange(slot.poked, false)) {
        slot.due_us = now;
    }
}

std::int64_t Core::due(Lane lane, std::int64_t now) const
{
    std::int64_t soonest = kNeverUs;
    for (const Slot &slot : jobs_) {
        if (slot.spec.lane == lane && can_run(slot)) {
            soonest = std::min(soonest, std::max(slot.due_us, now));
        }
    }
    return soonest;
}

Status Core::status(Job job, std::int64_t now) const
{
    Status out;
    if (job < 0 || job >= count()) {
        return out;
    }
    const Slot &slot = jobs_[job];
    out.waiting      = !slot.running && !can_run(slot);
    out.running      = slot.running;
    out.due_in_ms    = slot.due_us == kNeverUs ? -1 : static_cast<int>(std::max<std::int64_t>(slot.due_us - now, 0) / US_PER_MS);
    out.failures     = slot.failures;
    out.runs         = slot.runs;
    out.last_ms      = slot.last_ms;
    out.most_ms      = slot.most_ms;
    return out;
}

}  // namespace jobs
