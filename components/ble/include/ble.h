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

// What the link to the desk proxy costs, in microseconds, over the last few
// hundred exchanges. Measured rather than assumed: this decides whether a desk
// button can live at the other end of it.
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

// The desk, reached over the link rather than over a wire. Same shape as the
// loctek component it stands in for, so the supervisor above does not care
// which of the two it is talking to.
namespace ble::desk {

/** Called from the Bluetooth host task on every status the proxy sends. */
using StatusHandler = void (*)(int height_mm, bool linked, deskproto::Motion motion);

void on_status(StatusHandler handler);

bool connected();

/** Idle stops. Anything else is repeated until it is changed, and the proxy
 *  stops the desk if the repeats stop arriving. */
void hold(deskproto::Motion direction);

void preset(int index);
void store(int index);
void wake();

/** The last status the proxy sent. False if it has never sent one. */
bool last(int &height_mm, bool &box_linked, deskproto::Motion &motion);

}  // namespace ble::desk

namespace ble {

}  // namespace ble
