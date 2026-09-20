#pragma once

#include "esp_err.h"

namespace telemetry {

/** Connects to the broker and starts publishing what the panel knows. */
esp_err_t start();

/** Records a brightness change made on the panel, so it is published too. */
void note_brightness(int percent);

}  // namespace telemetry
