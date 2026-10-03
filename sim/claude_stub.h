#pragma once

#include <string>

// The laptops' Claude Code sessions, as tools/claude-hook posts them: the same
// events, taken by the same table, shown as claude_feed.cpp shows them. C steps
// through a few, and sim/send.sh claude sends any other.
namespace claude_stub {
/** One event, as the hook posts it to /claude. */
void from_hook(const std::string &payload);

/** Working, then one waiting on you, then all done, then none. */
void next_scene();
}  // namespace claude_stub
