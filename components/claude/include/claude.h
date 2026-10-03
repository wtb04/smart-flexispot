#pragma once

#include "claude_sessions.h"

#include <cstddef>
#include <cstdint>

// The one table of the laptops' sessions, which every task may ask of.
namespace claude {

/** One event from tools/claude-hook. False when it is not one. A session that
 *  has just begun to wait on you is copied to `asked`, with true in `was_asked`. */
bool take(const char *json, std::size_t length, Session &asked, bool &was_asked);

void snapshot(Snapshot &out);

/** The clock the sessions' times are on: milliseconds, steady. */
std::int64_t now_ms();

}  // namespace claude
