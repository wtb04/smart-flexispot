#pragma once

#include <cstddef>

// The laptops' Claude Code sessions, as tools/claude-hook tells Desk Link and
// Desk Link the panel, sealed.
namespace claude_feed {

/** One hook event, from the server's task. Quiet, a session that begins to
 *  wait on you shows as it does, without the chime and the notice. */
void take(const char *event, std::size_t length, bool quiet);

}  // namespace claude_feed
