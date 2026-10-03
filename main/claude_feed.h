#pragma once

#include "esp_err.h"

// The laptops' Claude Code sessions, as tools/claude-hook posts them to /claude.
namespace claude_feed {

/** On the update server, so after ota::start(). */
esp_err_t start();

}  // namespace claude_feed
