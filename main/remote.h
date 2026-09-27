#pragma once

#include "esp_err.h"

// A look at the panel from elsewhere, for the same key as its updates: its
// recent log and what is on its screen. Only in a build asking for it, with
// -DREMOTE_ENABLED=1; tools/remote.py fetches and saves them.
namespace remote {
/** After ota::start, whose server the pages go on. */
esp_err_t start();
}  // namespace remote
