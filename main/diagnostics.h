#pragma once

#include "esp_err.h"

#include <cstddef>

namespace diagnostics {

/** Starts the task that fills the Setup page's diagnostics view. */
esp_err_t start();

/** Refreshes now rather than on the next tick. Safe from the LVGL task. */
void refresh();

/** Recent log lines for a subsystem, named as the Setup page names it. */
void logs(const char *subsystem, char *out, std::size_t size);

}  // namespace diagnostics
