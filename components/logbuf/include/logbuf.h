#pragma once

#include "esp_err.h"

#include <cstddef>

namespace logbuf {
inline constexpr std::size_t kTextMax = 192;

/** One captured line, already stamped and joined. `level` is the logging
 *  system's own severity letter: E, W, I or D. */
struct Entry {
    char level;
    char text[kTextMax];
};

/** Maps a line's tag to the channel that keeps it, or -1 to discard it. */
using Router = int (*)(const char *tag);

/** Starts capturing everything written through the logging system; the console
 *  keeps receiving it too. Each channel gets its own ring, so a chatty
 *  subsystem cannot evict a quiet one. Call early: what happens before this is
 *  not kept. */
esp_err_t start(int channels, Router router);

/** How many lines a channel is holding. */
int count(int channel);

/** Reads one of a channel's lines, index 0 being the oldest it still holds.
 *  False if there is no such line. Taken one at a time so that showing them
 *  costs one Entry rather than a buffer of them. */
bool at(int channel, int index, Entry &out);

}  // namespace logbuf
