#pragma once

#include "esp_err.h"
#include "loctek_proto.h"

namespace loctek {
/** Called on the receive task. */
using HeightHandler = void (*)(int height_mm);

/** Pins and timings come from Kconfig. */
esp_err_t start(HeightHandler on_height);

/** Non-blocking, from any task. Move::Stop sends the release frame; going quiet
 *  would let the desk coast on. A hand on the keys ends any travel. */
esp_err_t request_move(Move direction);

/** Travels to a height the box has no preset for, steering on each height it
 *  reports: no clock, one decision per report. Releases early by the run-on
 *  learned from where earlier travels came to rest. Fails if the box has not
 *  said where it is yet. Bounded by the travel timeout, and by the height
 *  ceasing to change, as at the end of the desk's range. Any key sent
 *  afterwards ends it. */
esp_err_t goto_height(int height_mm);

/** The height being travelled to, or negative when not travelling. */
int driving_to();

/** The key being sent right now, as last requested. */
Move motion();

/** The box runs the move itself and ignores a plain stop while it does; sending
 *  the same preset again is what cancels it. */
esp_err_t goto_preset(Preset preset);

/** The M key, then the preset. M is sent exactly once: five seconds of it puts
 *  the control box into factory reset. */
esp_err_t store_preset(Preset preset);

esp_err_t send_key(Key key);

/** Pulses the wake line, then releases the keys. */
esp_err_t wake();

/** Wakes a sleeping panel with a single Up step: the box ignores the "no keys
 *  pressed" frame but always answers a real key. Moves the desk, so use it
 *  sparingly. */
esp_err_t nudge();

/** For telling "nothing on the wire" apart from "garbage on the wire". */
struct Stats {
    std::uint32_t bytes_received;
    std::uint32_t frames_decoded;
    std::uint32_t height_frames;  // type 0x12 seen, readable or not
    std::uint32_t heights_decoded;
};

Stats stats();

/** Copies out the most recent bytes seen on the wire, newest last, for when
 *  frames are not decoding and the question is what is arriving at all. */
int peek_raw(std::uint8_t *out, int capacity);

}  // namespace loctek
