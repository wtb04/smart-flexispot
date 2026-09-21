#pragma once

#include "esp_err.h"

#include <cstdint>

namespace ble {

struct Stats {
    bool ready         = false;
    bool has_key       = false;
    bool phone_present = false;  // recognised, recently, and close enough
    bool ever_seen     = false;
    int  phone_rssi    = -127;   // smoothed, which is what the thresholds act on
};

/** Call once, after Wi-Fi: the P4 has no radio, so the controller lives on the
 *  C6 and is driven over the same SDIO link Wi-Fi uses. */
esp_err_t start();

/** Thread-safe. */
Stats stats();

}  // namespace ble
