#pragma once

#include "esp_err.h"

#include <cstdint>

namespace board {
/** The backlight off and held so, before anything else, and until the first
 *  brightness is set: the panel shows flat blue while it has no picture. */
void dark_from_the_start();

/** The same, for a restart or a crash to leave behind. Safe in the panic handler. */
void hold_dark();

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

/** A development build's: interrupts held off on `core` for `ms`, as a long
 *  critical section would, and what the display's DMA did meanwhile. */
struct StallProbe {
    int           channel          = -1;
    int           core             = -1;
    int           moves            = 0;  // times its source address changed
    int           wraps            = 0;  // times it went back to a buffer's start
    std::int32_t  longest_still_us = 0;
    std::uint32_t first = 0, last = 0, fb0 = 0, fb1 = 0;
};
StallProbe probe_stall(int ms, int core);

/** How many frames the panel has been sent since it started, some 58 a
 *  second; fewer when something held its interrupt off. */
std::uint32_t refreshes();

/** Times the panel's frame interrupt came late: past about seven frames, the
 *  ring its DMA goes round by itself runs out, and the panel flickers blue.
 *  How many since it started, and the latest's gap and uptime. */
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
