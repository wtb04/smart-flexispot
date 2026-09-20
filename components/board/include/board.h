#pragma once

#include "esp_err.h"

namespace board {

/**
 * @brief Brings up power rails, panel, touch and the LVGL port task.
 *
 * Must be called once, before any lv_* call. Leaves the display rotated to
 * landscape with the backlight on.
 */
esp_err_t init();

/** Panel backlight, 0-100. */
esp_err_t set_brightness(int percent);

/** Same, shaped for ui::BrightnessHandler, which cannot report errors. */
void set_brightness_percent(int percent);

/** Brightness applied at startup. */
inline constexpr int kDefaultBrightness = 80;

/**
 * @brief Lowest backlight the panel actually responds to.
 *
 * Below this the display does not get any dimmer, so offering the range is
 * just a control that appears broken.
 */
inline constexpr int kMinBrightness = 20;

}  // namespace board
