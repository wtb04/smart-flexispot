#pragma once

#include "esp_err.h"

namespace board {
/** Brings up rails, panel, touch and the LVGL port task. Once, before any lv_*
 *  call. flipped hangs the panel the other way up. */
esp_err_t init(bool flipped);

/** Turns the picture and the touchscreen over together. Call with the LVGL
 *  lock held. */
/** From any task. */
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


/** Below this the panel does not get any dimmer, so offering the range is just a
 *  control that appears broken. */
inline constexpr int kMinBrightness = 20;

}  // namespace board
