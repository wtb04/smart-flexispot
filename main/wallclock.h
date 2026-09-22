#pragma once

#include "esp_err.h"

namespace wallclock {
esp_err_t start();

/** True once the clock has been set from the network, not sitting at 1970. */
bool synced();

}  // namespace wallclock
