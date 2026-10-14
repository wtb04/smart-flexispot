#pragma once

#include "esp_err.h"

namespace imu {
/** The BMI270, on the board's I2C bus. After the bus is up. */
esp_err_t start();

/** Acceleration in g along the chip's own axes; standing still it is gravity.
 *  False if there is no IMU or it would not answer. */
bool gravity(float &x, float &y, float &z);

/** The chip keeps its readings, 800 a second, until sharpest_jolt() takes
 *  them: a knock on the glass is a jump of a fraction of a g between two, where
 *  stillness is hundredths. */
esp_err_t watch_knocks();
void      stop_watching_knocks();

/** The largest jump, in g, between two readings kept since last asked. */
float sharpest_jolt();

}  // namespace imu
