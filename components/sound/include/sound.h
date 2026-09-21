#pragma once

#include "esp_err.h"

namespace sound {

esp_err_t init();

/** Returns immediately; safe from any task. */
void ding();

}  // namespace sound
