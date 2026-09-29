#pragma once

#include "esp_err.h"

#include <cstdint>

namespace board {
/** Brings up rails, panel, touch and the LVGL port task. Once, before any lv_*
 *  call. flipped hangs the panel the other way up. */
esp_err_t init(bool flipped);

/** Turns the picture and the touchscreen over together. From any task. */
void set_flipped(bool flipped);

/** Panel backlight, 0-100. */
esp_err_t set_brightness(int percent);

/** Shaped for ui::BrightnessHandler, which cannot report errors. */
void set_brightness_percent(int percent);

/** Lights the backlight. Kept out of init() so the panel stays dark until
 *  there is something on it worth seeing. */
esp_err_t display_on(int percent);

/** Backlight off and the panel asleep. The panel goes on scanning out whatever
 *  the MIPI link last left it, so anything that ends the program -- a restart,
 *  above all -- has to put it to sleep rather than only dim it. */
esp_err_t display_off();

/** How many frames the panel has been sent since it started, some 58 a
 *  second; fewer when something held its interrupt off. */
std::uint32_t refreshes();

/** Frames that came to the panel much later than their time: each a flicker
 *  of blue. How many since it started, and the latest's gap and uptime. */
struct LateFrames {
    std::uint32_t count;
    std::int32_t  gap_us;
    std::int64_t  at_us;
};
LateFrames late_frames();

/** How many times the panel's reads have fallen behind since it started: each
 *  a moment of blue screen, which the DSI driver reports only on the console. */
std::uint32_t underruns();

/** Below this the panel does not get any dimmer, so offering the range is just a
 *  control that appears broken. */
inline constexpr int kMinBrightness = 20;

}  // namespace board
