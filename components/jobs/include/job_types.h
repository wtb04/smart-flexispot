#pragma once

#include <cstdint>
#include <functional>

// What a component asks of jobs. Free of ESP-IDF, for the core and its tests.
namespace jobs {

using Job                   = int;
inline constexpr Job kNoJob = -1;

/** Which worker runs it. Quick ones never wait on anything slower than a
 *  lock or a bus read; a job that fetches is Slow, so a slow host holds up
 *  only the others that fetch. */
enum class Lane : std::uint8_t { Quick, Slow };

/** What a run came to, and so when it runs next. */
struct Result {
    bool ok      = true;
    int  next_ms = -1;  // -1: its period, or its back-off after a failure; kNever: when poked
};

inline constexpr int kNever = -2;

inline Result done() { return {}; }
/** Tried again after its back-off, which grows with each failure in a row. */
inline Result failed() { return {false, -1}; }
inline Result again_in(int ms) { return {true, ms}; }
/** Not again until poked. */
inline Result sleep() { return {true, kNever}; }

struct Spec {
    const char *name      = "";  // in the log and on /jobs
    Lane        lane      = Lane::Quick;
    int         period_ms = 0;  // 0: only when poked
    int         first_ms  = 0;  // after it is added; kNever: when first poked
    bool        online    = false;  // waits while there is no network
    bool        lit       = false;  // waits while the screen is dark
    int         retry_ms     = 30 * 1000;
    int         max_retry_ms = 5 * 60 * 1000;
    std::function<Result()> run;
};

struct Status {
    bool          waiting  = false;  // for the network or the screen
    bool          running  = false;
    int           due_in_ms = 0;     // -1 when only poked
    int           failures = 0;      // in a row
    std::uint32_t runs     = 0;
    int           last_ms  = 0;      // how long its last run took
    int           most_ms  = 0;      // and its longest
};

}  // namespace jobs
