#pragma once

#include "watchdog_core.h"

// A shared worker that takes too long over one thing is stuck, and a stuck
// worker is a panel that has quietly stopped doing part of its job. Found from
// a timer, not by the workers, which cannot notice their own hang: it says who
// was stuck on what, and crashes, so the restart keeps it in its last words.
//
//     static const watchdog::Beat beat = watchdog::add("jobs", 10 * 1000);
//     watchdog::busy(beat, job_name);
//     run();
//     watchdog::idle(beat);
namespace watchdog {

/** Thread-safe; the first starts the timer that looks. */
Beat add(const char *who, int limit_ms);
void busy(Beat beat, const char *what);
void idle(Beat beat);

}  // namespace watchdog
