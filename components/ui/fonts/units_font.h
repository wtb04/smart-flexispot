#pragma once

#include "lvgl.h"

extern "C" {
LV_FONT_DECLARE(lv_font_units_16)
LV_FONT_DECLARE(lv_font_units_20)
LV_FONT_DECLARE(lv_font_units_22)
LV_FONT_DECLARE(lv_font_units_28)
LV_FONT_DECLARE(lv_font_units_32)
LV_FONT_DECLARE(lv_font_units_48)

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
