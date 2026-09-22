#pragma once

#include "esp_err.h"

#include <ctime>

namespace rtc {

/** Requires bsp_i2c_init() first. Reads the backup clock and, if it is holding
 *  a time worth believing, sets the system clock from it -- so the panel shows
 *  the right time on the first frame rather than whenever the network answers.
 *  Also switches the chip's backup supply on, without which it forgets the time
 *  the moment main power goes. */
esp_err_t start();

/** Whether the chip is there and its time survived the last power cut. */
bool holding_time();

/** Written back whenever the network settles the time, so the next cold boot
 *  starts from something better than 1970. */
esp_err_t store(std::time_t when);

}  // namespace rtc
