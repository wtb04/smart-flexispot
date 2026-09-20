#pragma once

#include "esp_err.h"
#include "loctek_proto.h"

namespace loctek {

enum class Move : std::int8_t {
    Stop = 0,
    Up   = 1,
    Down = -1,
};

/** Invoked from the receive task whenever the desk reports a new height. */
using HeightHandler = void (*)(int height_mm);

/**
 * @brief Opens the UART to the control box and starts the transmit and receive
 *        tasks. Pins and timings come from Kconfig.
 */
esp_err_t start(HeightHandler on_height);

/**
 * @brief Requests a direction of travel.
 *
 * The desk moves one short step per frame it receives, so the transmit task
 * repeats the key frame while a direction is held. Move::Stop sends the release
 * frame immediately -- just going quiet lets the desk coast on.
 * Non-blocking; safe from any task.
 */
esp_err_t request_move(Move direction);

/**
 * @brief Sends the desk to a stored preset.
 *
 * The box runs the move itself and ignores a plain stop while it does, so
 * sending the same preset again is what cancels it -- which is what the
 * handset does.
 */
esp_err_t goto_preset(Preset preset);

/**
 * @brief Stores the current height in a preset: the M key, then the preset.
 *
 * M is sent exactly once. Holding it for five seconds puts the control box
 * into factory reset, so it must never go on a repeat timer.
 */
esp_err_t store_preset(Preset preset);

/** Sends one key frame directly. */
esp_err_t send_key(Key key);

/**
 * @brief Asks a sleeping control box for a height.
 *
 * A "no keys pressed" frame does not wake the panel, but a real key press does.
 * This sends a single Up frame followed immediately by the release frame: one
 * frame is the smallest step the desk can take, audible as a click but not
 * visible, and it makes the box light its display and start reporting.
 */
esp_err_t wake();

/**
 * @brief Wakes a sleeping panel with a single Up step.
 *
 * The box ignores the "no keys pressed" frame but always answers a real key.
 * One frame is the smallest movement the desk can make: a click rather than a
 * visible move. Moves the desk, so call it sparingly.
 */
esp_err_t nudge();

/**
 * @brief Link counters, for telling "nothing on the wire" apart from "garbage
 *        on the wire" when bringing the wiring up.
 */
struct Stats {
    std::uint32_t bytes_received;
    std::uint32_t frames_decoded;
    std::uint32_t height_frames;    // type 0x12 seen, readable or not
    std::uint32_t heights_decoded;  // of those, ones carrying a real number
};

Stats stats();

}  // namespace loctek
