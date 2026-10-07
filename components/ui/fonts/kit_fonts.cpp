#include "kit_fonts.h"

extern "C" {
LV_FONT_DECLARE(lv_font_units_16)
LV_FONT_DECLARE(lv_font_units_20)
LV_FONT_DECLARE(lv_font_units_22)
LV_FONT_DECLARE(lv_font_units_28)
LV_FONT_DECLARE(lv_font_units_32)
}

namespace ui::kit::fonts {
namespace {
// Instrument Sans has no µ, ±, ², ³ or fractions, and lacks some of Latin
// Extended-A; Montserrat's units fonts carry them, at the nearest size they come in.
lv_font_t with_fallback(const lv_font_t &base, const lv_font_t &fallback)
{
    lv_font_t copy = base;
    copy.fallback  = &fallback;
    return copy;
}

}  // namespace

const lv_font_t *label()
{
    static const lv_font_t font = with_fallback(lv_font_kit_label, lv_font_units_16);
    return &font;
}

const lv_font_t *small()
{
    static const lv_font_t font = with_fallback(lv_font_kit_small, lv_font_units_16);
    return &font;
}

const lv_font_t *body()
{
    static const lv_font_t font = with_fallback(lv_font_kit_body, lv_font_units_20);
    return &font;
}

const lv_font_t *h3()
{
    static const lv_font_t font = with_fallback(lv_font_kit_h3, lv_font_units_22);
    return &font;
}

const lv_font_t *h2()
{
    static const lv_font_t font = with_fallback(lv_font_kit_h2, lv_font_units_28);
    return &font;
}

const lv_font_t *h1()
{
    static const lv_font_t font = with_fallback(lv_font_kit_h1, lv_font_units_32);
    return &font;
}

const lv_font_t *read_s()
{
    static const lv_font_t font = with_fallback(lv_font_kit_read_s, lv_font_units_28);
    return &font;
}

const lv_font_t *read_m() { return &lv_font_kit_read_m; }
const lv_font_t *read_l() { return &lv_font_kit_read_l; }
const lv_font_t *read_xl() { return &lv_font_kit_read_xl; }
const lv_font_t *read_xxl() { return &lv_font_kit_read_xxl; }

const lv_font_t *icon(int size, bool filled)
{
    if (size < 26) {
        return filled ? &lv_font_kit_icon_f_24 : &lv_font_kit_icon_o_24;
    }
    if (size < 31) {
        return filled ? &lv_font_kit_icon_f_28 : &lv_font_kit_icon_o_28;
    }
    return filled ? &lv_font_kit_icon_f_34 : &lv_font_kit_icon_o_34;
}

}  // namespace ui::kit::fonts
