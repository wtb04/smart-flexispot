#pragma once

#include "esp_err.h"
#include "loctek_proto.h"

namespace loctek {
/** Called on the receive task with each height the box reports. Store it and
 *  notify; never take a lock or write flash here. */
using HeightHandler = void (*)(int height_mm);

/** Pins, timings and the desk's range come from Kconfig. One driver task
 *  writes every key frame; the calls below only tell it what is wanted. */
esp_err_t start(HeightHandler on_height);

/** A hand on the keys. Non-blocking, from any task. Only the latest request
 *  matters, so a Stop can never queue behind anything. Move::Stop sends the
 *  release frame; going quiet would let the desk coast on. Ends any travel, and
 *  no travel can start until the hand lets go. */
esp_err_t request_move(Move direction);

/** Travels to a height the box has no preset for, steering on each height it
 *  reports: no clock, one decision per report. Releases early by the run-on
 *  learned from where earlier travels came to rest. Refused while a hand is on
 *  the keys, when the target is outside the desk's range, or when the box has
 *  not said where it is in the last two seconds. Bounded by the travel timeout,
 *  by the height ceasing to change, and by the desk moving away from the
 *  target. Any key sent afterwards ends it. A new target replaces the old. */
esp_err_t goto_height(int height_mm);

/** The height being travelled to, or negative when not travelling. */
int driving_to();

/** The key being sent right now. */
Move motion();

/** True for a height the desk can actually reach. */
bool in_range(int height_mm);

/** The box runs the move itself and ignores a plain stop while it does; sending
 *  the same preset again is what cancels it. Ends any travel first. */
esp_err_t goto_preset(Preset preset);

/** The M key, then the preset. M is sent exactly once: five seconds of it puts
 *  the control box into factory reset. */
esp_err_t store_preset(Preset preset);

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
