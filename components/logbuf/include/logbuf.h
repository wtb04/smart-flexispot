#pragma once

#include "esp_err.h"

#include <cstddef>
#include <cstdint>

namespace logbuf {
inline constexpr std::size_t kTextMax = 192;

/** One captured line, already stamped and joined. `level` is the logging
 *  system's own severity letter: E, W, I or D. */
struct Entry {
    char level;
    char text[kTextMax];
    int  channel;
};

/** Maps a line's tag to the channel that keeps it, or -1 to discard it. */
using Router = int (*)(const char *tag);

/** Starts capturing everything written through the logging system; the console
 *  keeps receiving it too. Each channel gets its own ring, so a chatty
 *  subsystem cannot evict a quiet one. Call early: what happens before this is
 *  not kept. */
esp_err_t start(int channels, Router router);

/** The most recent lines across the channels in `mask`, one bit each, merged
 *  into the order they were logged, oldest first; with `warnings`, only E and
 *  W. Returns how many were written to `out`, at most `max`. */
int recent(std::uint32_t mask, bool warnings, Entry *out, int max);

}  // namespace logbuf
