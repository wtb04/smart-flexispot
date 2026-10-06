#pragma once

#include "kit_icons.h"
#include "lvgl.h"

extern "C" {
LV_FONT_DECLARE(lv_font_kit_label)
LV_FONT_DECLARE(lv_font_kit_small)
LV_FONT_DECLARE(lv_font_kit_body)
LV_FONT_DECLARE(lv_font_kit_h3)
LV_FONT_DECLARE(lv_font_kit_h2)
LV_FONT_DECLARE(lv_font_kit_h1)
LV_FONT_DECLARE(lv_font_kit_read_s)
LV_FONT_DECLARE(lv_font_kit_read_m)
LV_FONT_DECLARE(lv_font_kit_read_l)
LV_FONT_DECLARE(lv_font_kit_read_xl)
LV_FONT_DECLARE(lv_font_kit_read_xxl)

LV_FONT_DECLARE(lv_font_kit_icon_o_24)
LV_FONT_DECLARE(lv_font_kit_icon_o_28)
LV_FONT_DECLARE(lv_font_kit_icon_o_34)
LV_FONT_DECLARE(lv_font_kit_icon_f_24)
LV_FONT_DECLARE(lv_font_kit_icon_f_28)
LV_FONT_DECLARE(lv_font_kit_icon_f_34)
}

/** Instrument Sans, its figures all one width, and Material Symbols Rounded. See tools/make_fonts.py. */
namespace ui::kit::fonts {
const lv_font_t *label();   // semibold 13
const lv_font_t *small();   // medium 16
const lv_font_t *body();    // medium 19
const lv_font_t *h3();      // semibold 21
const lv_font_t *h2();      // semibold 26
const lv_font_t *h1();      // semibold 34
const lv_font_t *read_s();  // semibold 28

/** Figures and " +,-.:%°−" only; a unit beside one goes in a smaller font. */
const lv_font_t *read_m();    // 40
const lv_font_t *read_l();    // 56
const lv_font_t *read_xl();   // 76
const lv_font_t *read_xxl();  // 160

/** The nearest of 24, 28 and 34, for the strings in ui::kit::glyph. */
const lv_font_t *icon(int size, bool filled = false);
}  // namespace ui::kit::fonts
