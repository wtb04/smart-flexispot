#pragma once

#include "esp_err.h"

namespace wallclock {

/** Starts pushing the local time to the display once it is known. */
esp_err_t start();

/** True once the clock has been set from the network rather than sitting at 1970. */
bool synced();

}  // namespace wallclock
