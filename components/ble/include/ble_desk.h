#pragma once

#include "esp_err.h"

#include <cstddef>
#include <cstdint>

// The panel's side of the link to the desk companion. It shares the radio, the
// host and the scanner with the phone presence in ble.h, which starts them.

#include "deskproto.h"

namespace ble {
struct LinkStats {
    bool connected;
    int  samples;
    int  min_us;
    int  median_us;
    int  p99_us;
    int  max_us;
    int  lost;
};

/** How the link to the companion is doing, for the diagnostics page. */
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

/** Milliseconds since the proxy last said anything, or negative when there is
 *  no link. */
int quiet_ms();

/** The last status the proxy sent. False if it has never sent one. */
bool last(deskproto::Status &out);

/** Sends the companion a new firmware, piece by piece, each answered before the
 *  next, and returns once it has been taken or refused; the companion restarts
 *  into it afterwards, so the link drops. progress, when given, hears 0 to 100.
 *  Not from the NimBLE host task. */
esp_err_t send_update(const std::uint8_t *image, std::size_t size, std::uint32_t crc,
                      void (*progress)(int percent));

}  // namespace ble::desk
