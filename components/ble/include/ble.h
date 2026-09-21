#pragma once

#include "esp_err.h"

#include <cstdint>

namespace ble {

/** Whether the tracked phone is in the room. */
struct Stats {
    bool ready         = false;  // controller up and scanning
    bool has_key       = false;  // an identity key is configured at all
    bool phone_present = false;  // recognised, recently, and close enough
    bool ever_seen     = false;  // recognised at least once since boot
    int  phone_rssi    = -127;   // smoothed, which is what the thresholds act on
};

/**
 * @brief Starts the BLE observer. Call once.
 *
 * The ESP32-P4 has no radio: the controller lives on the C6 co-processor and
 * this drives it over the same SDIO link Wi-Fi uses, so Wi-Fi must be up first.
 */
esp_err_t start();

/** A snapshot of the tracked phone. Thread-safe. */
Stats stats();

}  // namespace ble
