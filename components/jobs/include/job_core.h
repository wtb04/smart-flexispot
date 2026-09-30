#pragma once

#include "job_types.h"

#include <cstdint>
#include <vector>

// Which job runs next on a lane: the one due soonest, among those whose
// network and screen are there, one at a time each. Pure: it is told the time
// and what came of each run, so the host tests drive it as the panel does.
// Not thread-safe; whoever drives it keeps it under a lock.
namespace jobs {

class Core {
public:
    static constexpr std::int64_t kNeverUs = INT64_MAX;

    Job  add(Spec spec, std::int64_t now);
    void poke(Job job, std::int64_t now);
    void conditions(bool online, bool lit);

    /** The job to run now on `lane`, marked running; false for none. */
    bool next(Lane lane, std::int64_t now, Job &out);
    void finish(Job job, const Result &result, int took_ms, std::int64_t now);

    /** When `lane` next has something to run, if nothing changes before. */
    std::int64_t due(Lane lane, std::int64_t now) const;

    const Spec &spec(Job job) const { return jobs_[job].spec; }
    int         count() const { return static_cast<int>(jobs_.size()); }
    Status      status(Job job, std::int64_t now) const;

private:
    struct Slot {
        Spec          spec;
        std::int64_t  due_us   = kNeverUs;
        bool          running  = false;
        bool          poked    = false;  // while running: again as soon as it finishes
        int           failures = 0;
        std::uint32_t runs     = 0;
        int           last_ms  = 0;
        int           most_ms  = 0;
    };

    bool can_run(const Slot &slot) const;

    std::vector<Slot> jobs_;
    bool              online_ = false;
    bool              lit_    = true;
};

}  // namespace jobs
