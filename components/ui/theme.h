#pragma once

#include "lvgl.h"

#include <cstdint>
#include <cstring>

// Orange on near-black, carried over from the earlier panel so the two read as
// the same product.
namespace ui::theme {

inline constexpr std::uint32_t background  = 0x15110f;
inline constexpr std::uint32_t panel       = 0x241c18;
inline constexpr std::uint32_t panel_light = 0x382820;
inline constexpr std::uint32_t text        = 0xfff3ea;
inline constexpr std::uint32_t secondary   = 0xb7a092;
inline constexpr std::uint32_t orange      = 0xff8738;
inline constexpr std::uint32_t green       = 0x8bcb78;
inline constexpr std::uint32_t amber       = 0xffc15a;
inline constexpr std::uint32_t red         = 0xff6666;

// Writing a label invalidates its area whether or not the text changed, and
// these are driven from timers. Comparing first means a screen that is not
// changing costs the renderer nothing.
inline void set_text(lv_obj_t *label, const char *value)
{
    if (label == nullptr || value == nullptr) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current != nullptr && std::strcmp(current, value) == 0) {
        return;
    }
    lv_label_set_text(label, value);
}

// Same reasoning: setting a style marks the object dirty even when the value
// is identical to what is already there.
inline void set_text_color(lv_obj_t *obj, std::uint32_t colour)
{
    if (obj == nullptr) {
        return;
    }
    const lv_color_t next = lv_color_hex(colour);
    if (lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), next)) {
        return;
    }
    lv_obj_set_style_text_color(obj, next, 0);
}

inline void style_panel(lv_obj_t *obj, std::uint32_t colour = panel, int radius = 16)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(colour), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollable(obj, false);
}

inline void style_button(lv_obj_t *button, std::uint32_t colour = panel_light)
{
    style_panel(button, colour, 14);
    lv_obj_set_style_bg_color(button, lv_color_hex(orange), LV_STATE_PRESSED);
    // LVGL's default button style carries a shadow and an outline that fight a
    // flat panel look; without clearing them the buttons read as embossed.
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(button, 0, 0);
    lv_obj_set_style_outline_width(button, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 0, LV_STATE_PRESSED);
    // Pressed state changes colour only, with no shift in geometry.
    lv_obj_set_style_translate_y(button, 0, LV_STATE_PRESSED);
}

inline lv_obj_t *make_label(lv_obj_t *parent, const char *value, std::uint32_t colour = text,
                            const lv_font_t *font = &lv_font_montserrat_20)
{
    lv_obj_t *label = lv_label_create(parent);
    set_text(label, value);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

inline lv_obj_t *make_button(lv_obj_t *parent, const char *value,
                             std::uint32_t colour  = panel_light,
                             const lv_font_t *font = &lv_font_montserrat_28)
{
    lv_obj_t *button = lv_button_create(parent);
    style_button(button, colour);
    lv_obj_center(make_label(button, value, text, font));
    return button;
}

// Page chrome, so every screen lines up identically.
inline lv_obj_t *make_page_title(lv_obj_t *parent, const char *value)
{
    lv_obj_t *label = make_label(parent, value, text, &lv_font_montserrat_32);
    lv_obj_set_pos(label, 32, 22);
    return label;
}

inline lv_obj_t *make_card_heading(lv_obj_t *parent, const char *value)
{
    lv_obj_t *label = make_label(parent, value, orange, &lv_font_montserrat_16);
    lv_obj_set_pos(label, 20, 16);
    return label;
}

}  // namespace ui::theme
