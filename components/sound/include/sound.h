#pragma once

#include "esp_err.h"

namespace sound {
esp_err_t init();

/** 0 silences it. Takes effect on the next chime. */
void set_volume(int percent);

/** Returns immediately; safe from any task. */
void ding();

}  // namespace sound
