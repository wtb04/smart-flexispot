#pragma once

#include "esp_err.h"

namespace orientation {
/** Watches the IMU and turns the screen when the panel is stood the other way
 *  up, while the Auto setting is on. */
esp_err_t start();

/** Auto was just picked: whatever is upright now is taken as upright, if the
 *  panel is standing. Then looks at once rather than on the next tick. */
void calibrate_and_refresh();

}  // namespace orientation
