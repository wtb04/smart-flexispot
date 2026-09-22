#include "units_font.h"

namespace ui::fonts {
namespace {
lv_font_t s_16;
lv_font_t s_20;
lv_font_t s_22;
lv_font_t s_28;
lv_font_t s_32;
lv_font_t s_48;

void attach(lv_font_t &copy, const lv_font_t &base, const lv_font_t &fallback)
{
    copy          = base;
    copy.fallback = &fallback;
}

}  // namespace

void init()
{
    attach(s_16, lv_font_montserrat_16, lv_font_units_16);
    attach(s_20, lv_font_montserrat_20, lv_font_units_20);
    attach(s_22, lv_font_montserrat_22, lv_font_units_22);
    attach(s_28, lv_font_montserrat_28, lv_font_units_28);
    attach(s_32, lv_font_montserrat_32, lv_font_units_32);
    attach(s_48, lv_font_montserrat_48, lv_font_units_48);
}

const lv_font_t *size_16() { return &s_16; }
const lv_font_t *size_20() { return &s_20; }
const lv_font_t *size_22() { return &s_22; }
const lv_font_t *size_28() { return &s_28; }
const lv_font_t *size_32() { return &s_32; }
const lv_font_t *size_48() { return &s_48; }

const lv_font_t *temp_34() { return &lv_font_temp_34; }
const lv_font_t *temp_64() { return &lv_font_temp_64; }

}  // namespace ui::fonts
