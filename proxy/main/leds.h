#pragma once

#include "esp_err.h"

namespace deskled {

/** Drives the two indicator LEDs built into the RJ45 jack.
 *
 *  BT   (CONFIG_PROXY_LED_BT_GPIO,   'BT' on the silkscreen)   - the panel link
 *  LINK (CONFIG_PROXY_LED_DESK_GPIO, 'LINK' on the silkscreen) - the control box
 *
 *  Both are active high: the GPIO drives a 220R series resistor into the jack's
 *  LED anode, cathode to GND. Set either GPIO to -1 to leave that pin alone.
 *
 *  The LEDs live inside the connector housing, so there is no way to fit them
 *  backwards -- but the board has to drive the right pin of the pair as the
 *  anode. If one never lights, that is the thing to check, not this code. */
esp_err_t start();

/** Blinks each LED on its own, then both together. Lets a loose LED clipped to
 *  one pin prove that pin before the board exists, and makes a swapped pair
 *  obvious. Blocks for about 2 s. */
void selftest();

}  // namespace deskled
