#pragma once

#include "deskproto.h"
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

struct LinkStats {
    bool connected;
    int  samples;
    int  min_us;
    int  median_us;
    int  p99_us;
    int  max_us;
    int  lost;
};

LinkStats link_stats();

}  // namespace ble

namespace ble::desk {
/** Called from the Bluetooth host task on every status the proxy sends. */
using StatusHandler = void (*)(const deskproto::Status &status);

void on_status(StatusHandler handler);

bool connected();

/** Idle stops. Anything else is repeated until it is changed, and the proxy
 *  stops the desk if the repeats stop arriving. */
void hold(deskproto::Motion direction);

void preset(int index);
void store(int index);
void wake();

/** The proxy drives the desk to this height itself and reports as it goes. */
void goto_height(int height_mm);

/** Lets go of everything, hold or travel, whether or not this panel was holding. */
void stop();

/** The last status the proxy sent. False if it has never sent one. */
bool last(deskproto::Status &out);

}  // namespace ble::desk

namespace ble {
}  // namespace ble
