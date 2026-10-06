#pragma once

#include "esp_err.h"

namespace dark {
/** While the screen is dark, the panel sleeps deeper and a knock on the glass
 *  wakes it. After the IMU and the network have started. */
esp_err_t start();

}  // namespace dark
