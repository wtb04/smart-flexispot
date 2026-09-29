#pragma once

// What the panel was doing when it last went down without being asked to:
// its last log lines and, after a crash, where. Kept in memory that a restart
// leaves alone, and logged once it is back, so the log shows what a blue
// screen and a restart otherwise hide.
namespace last_words {
/** Logs why the panel started and what the run before left behind, then
 *  starts keeping this run's. After logbuf::start, so that it keeps both. */
void start();

/** What start() logged, for as long as this run lasts. */
const char *report();
}  // namespace last_words
