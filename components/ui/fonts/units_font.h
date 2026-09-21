#pragma once

#include "lvgl.h"

// Generated with lv_font_conv, containing only the characters LVGL's built-in
// Montserrat build lacks: the micro sign and Greek mu, superscript two and
// three, and the combined degree-Celsius and degree-Fahrenheit glyphs.
//
// These hang off the Montserrat fonts as a fallback rather than replacing
// them, so all ordinary text keeps its typeface and only the units a sensor
// reports come from elsewhere.
extern "C" {
LV_FONT_DECLARE(lv_font_units_16)
LV_FONT_DECLARE(lv_font_units_20)
LV_FONT_DECLARE(lv_font_units_22)
LV_FONT_DECLARE(lv_font_units_28)
LV_FONT_DECLARE(lv_font_units_32)
LV_FONT_DECLARE(lv_font_units_48)

// Montserrat SemiBold, digits and the degree sign only: the temperature
// readout is the one place on the panel that wants weight and a size the
// built-in Montserrat does not go up to.
LV_FONT_DECLARE(lv_font_temp_34)
LV_FONT_DECLARE(lv_font_temp_64)
}

namespace ui::fonts {

/** Montserrat with the units fallback attached. Call once, before any label. */
void init();

const lv_font_t *size_16();
const lv_font_t *size_20();
const lv_font_t *size_22();
const lv_font_t *size_28();
const lv_font_t *size_32();
const lv_font_t *size_48();

/** Digits only, semibold. Anything else in a label using these comes out blank. */
const lv_font_t *temp_34();
const lv_font_t *temp_64();

}  // namespace ui::fonts
