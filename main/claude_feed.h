#pragma once

#include <cstddef>

// The laptops' Claude Code sessions, as tools/claude-hook tells Desk Link and
// Desk Link the panel, sealed.
namespace claude_feed {

/** One hook event, from the server's task. */
void take(const char *event, std::size_t length);

}  // namespace claude_feed
