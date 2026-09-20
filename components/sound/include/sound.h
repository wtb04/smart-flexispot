#pragma once

#include "esp_err.h"

namespace sound {

/** Powers the speaker and opens the codec. */
esp_err_t init();

/** Plays the notification chime. Returns immediately; safe from any task. */
void ding();

}  // namespace sound
