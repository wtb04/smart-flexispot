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

}  // namespace board
