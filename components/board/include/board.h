#pragma once

#include "esp_err.h"

namespace board {

/** Brings up rails, panel, touch and the LVGL port task. Once, before any lv_*
 *  call. */
esp_err_t init();

/** Panel backlight, 0-100. */
esp_err_t set_brightness(int percent);

/** Shaped for ui::BrightnessHandler, which cannot report errors. */
void set_brightness_percent(int percent);

inline constexpr int kDefaultBrightness = 80;

/** Below this the panel does not get any dimmer, so offering the range is just a
 *  control that appears broken. */
inline constexpr int kMinBrightness = 20;

}  // namespace board
