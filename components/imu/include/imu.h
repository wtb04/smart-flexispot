#pragma once

#include "esp_err.h"

namespace imu {
/** The BMI270, on the board's I2C bus. After the bus is up. */
esp_err_t start();

/** Acceleration in g along the chip's own axes; standing still it is gravity.
 *  False if there is no IMU or it would not answer. */
bool gravity(float &x, float &y, float &z);

}  // namespace imu
