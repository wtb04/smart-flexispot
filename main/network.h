#pragma once

#include "esp_err.h"

namespace network {
/** Waits for an address, starts every network client with what it reports to,
 *  keeps them connected, and publishes what the panel knows every two seconds. */
esp_err_t start();

/** Records a brightness change made on the panel, so it is published too. */
void note_brightness(int percent);

/** The panel's own screen control, so Home Assistant sees it too. */
void note_screen(bool on);

}  // namespace network
