#pragma once

#include "lvgl.h"

// Generated with lv_font_conv for what LVGL's built-in Montserrat build lacks: the text sizes
// carry Latin-1, Latin Extended-A and the typographic punctuation real track titles are full of,
// the larger sizes only symbols and numbers. The degree glyphs come from a symbol font, the rest
// from Montserrat itself. These hang off the Montserrat fonts as a fallback rather than replacing
// them, so ordinary text keeps its typeface.
extern "C" {
LV_FONT_DECLARE(lv_font_units_16)
LV_FONT_DECLARE(lv_font_units_20)
LV_FONT_DECLARE(lv_font_units_22)
LV_FONT_DECLARE(lv_font_units_28)
LV_FONT_DECLARE(lv_font_units_32)
LV_FONT_DECLARE(lv_font_units_48)

// Montserrat SemiBold, digits and the degree sign only, at sizes the built-in font does not reach.
LV_FONT_DECLARE(lv_font_temp_34)
LV_FONT_DECLARE(lv_font_temp_64)
}

namespace ui::fonts {

/** Call once, before any label is created. */
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
