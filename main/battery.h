#pragma once

#include "esp_err.h"

namespace battery {

/** Starts polling the power monitor and pushing it to the display. */
esp_err_t start();

}  // namespace battery
