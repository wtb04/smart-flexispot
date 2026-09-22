#pragma once

#include "lvgl.h"
#include "units_font.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace ui::theme {
inline constexpr std::uint32_t background  = 0x15110f;
inline constexpr std::uint32_t panel       = 0x241c18;
inline constexpr std::uint32_t panel_light = 0x382820;
inline constexpr std::uint32_t disabled     = 0x1b1512;
inline constexpr std::uint32_t disabled_ink = 0x5c4d43;
inline constexpr std::uint32_t text        = 0xfff3ea;
inline constexpr std::uint32_t secondary   = 0xb7a092;
inline constexpr std::uint32_t green       = 0x8bcb78;
inline constexpr std::uint32_t amber       = 0xffc15a;
inline constexpr std::uint32_t red         = 0xff6666;

inline constexpr std::uint32_t default_primary = 0xff8738;
inline constexpr std::array<std::uint32_t, 10> primaries{
    0xff6b5b, default_primary, 0xffb44a, 0xa8d75a, 0x5fcf7a,
    0x3fd0c0, 0x4fb0f5, 0x8b93ff, 0xb579f0, 0xff6fd0,
};

inline constexpr std::uint32_t dim_of(std::uint32_t colour)
{
    constexpr std::uint32_t PART = 78;
    return ((((colour >> 16) & 0xff) * PART / 100) << 16) |
           ((((colour >> 8) & 0xff) * PART / 100) << 8) | ((colour & 0xff) * PART / 100);
}

inline std::uint32_t primary     = default_primary;
inline std::uint32_t primary_dim = dim_of(default_primary);
inline std::uint32_t &orange     = primary;
inline std::uint32_t &orange_dim = primary_dim;

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

inline bool has_local_color(lv_obj_t *obj, lv_style_prop_t prop, lv_style_selector_t selector,
                            lv_color_t next)
{
    lv_style_value_t current;
    return lv_obj_get_local_style_prop(obj, prop, &current, selector) == LV_STYLE_RES_FOUND &&
           lv_color_eq(current.color, next);
}

inline void set_text_color(lv_obj_t *obj, std::uint32_t colour)
{
    if (obj == nullptr) {
        return;
    }
    const lv_color_t next = lv_color_hex(colour);
    if (has_local_color(obj, LV_STYLE_TEXT_COLOR, 0, next)) {
        return;
    }
    lv_obj_set_style_text_color(obj, next, 0);
}

inline void set_bg_color(lv_obj_t *obj, std::uint32_t colour, lv_style_selector_t selector = 0)
{
    if (obj == nullptr) {
        return;
    }
    const lv_color_t next = lv_color_hex(colour);
    if (has_local_color(obj, LV_STYLE_BG_COLOR, selector, next)) {
        return;
    }
    lv_obj_set_style_bg_color(obj, next, selector);
}

inline void set_arc_color(lv_obj_t *obj, std::uint32_t colour, lv_style_selector_t selector)
{
    if (obj == nullptr) {
        return;
    }
    const lv_color_t next = lv_color_hex(colour);
    if (has_local_color(obj, LV_STYLE_ARC_COLOR, selector, next)) {
        return;
    }
    lv_obj_set_style_arc_color(obj, next, selector);
}

inline lv_style_t accent_fill_style;
inline lv_style_t accent_dim_style;
inline lv_style_t accent_ink_style;
inline lv_style_t accent_arc_style;

inline void set_primary(std::uint32_t colour)
{
    primary     = colour;
    primary_dim = dim_of(colour);
    lv_style_set_bg_color(&accent_fill_style, lv_color_hex(primary));
    lv_style_set_bg_color(&accent_dim_style, lv_color_hex(primary_dim));
    lv_style_set_text_color(&accent_ink_style, lv_color_hex(primary));
    lv_style_set_arc_color(&accent_arc_style, lv_color_hex(primary));
    lv_obj_report_style_change(nullptr);
}

/** Once, before anything is built. */
inline void init_accents()
{
    for (lv_style_t *style :
         {&accent_fill_style, &accent_dim_style, &accent_ink_style, &accent_arc_style}) {
        lv_style_init(style);
    }
    set_primary(primary);
}

inline void fill_accent(lv_obj_t *obj, lv_style_selector_t selector = 0)
{
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_local_style_prop(obj, LV_STYLE_BG_COLOR, selector);
    lv_obj_add_style(obj, &accent_fill_style, selector);
}

inline void fill_dim_accent(lv_obj_t *obj, lv_style_selector_t selector)
{
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_local_style_prop(obj, LV_STYLE_BG_COLOR, selector);
    lv_obj_add_style(obj, &accent_dim_style, selector);
}

inline void ink_accent(lv_obj_t *obj, lv_style_selector_t selector = 0)
{
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_COLOR, selector);
    lv_obj_add_style(obj, &accent_ink_style, selector);
}

inline void arc_accent(lv_obj_t *obj, lv_style_selector_t selector)
{
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_local_style_prop(obj, LV_STYLE_ARC_COLOR, selector);
    lv_obj_add_style(obj, &accent_arc_style, selector);
}

inline void fill_accent_or(lv_obj_t *obj, bool accent, std::uint32_t colour,
                           lv_style_selector_t selector = 0)
{
    if (accent) {
        fill_accent(obj, selector);
        return;
    }
    lv_obj_remove_style(obj, &accent_fill_style, selector);
    set_bg_color(obj, colour, selector);
}

inline void ink_accent_or(lv_obj_t *obj, bool accent, std::uint32_t colour)
{
    if (accent) {
        ink_accent(obj);
        return;
    }
    lv_obj_remove_style(obj, &accent_ink_style, 0);
    set_text_color(obj, colour);
}

inline void arc_accent_or(lv_obj_t *obj, bool accent, std::uint32_t colour,
                          lv_style_selector_t selector)
{
    if (accent) {
        arc_accent(obj, selector);
        return;
    }
    lv_obj_remove_style(obj, &accent_arc_style, selector);
    set_arc_color(obj, colour, selector);
}

inline void align(lv_obj_t *obj, lv_align_t alignment, std::int32_t x, std::int32_t y)
{
    if (obj == nullptr) {
        return;
    }
    lv_style_value_t current;
    if (lv_obj_get_local_style_prop(obj, LV_STYLE_ALIGN, &current, 0) != LV_STYLE_RES_FOUND ||
        current.num != static_cast<std::int32_t>(alignment)) {
        lv_obj_set_style_align(obj, alignment, 0);
    }
    lv_obj_set_pos(obj, x, y);
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

inline void hand_down_press(lv_event_t *event)
{
    lv_obj_t  *button  = lv_event_get_current_target_obj(event);
    const bool pressed = lv_event_get_code(event) == LV_EVENT_PRESSED;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_t *child = lv_obj_get_child(button, i);
        if (pressed) {
            lv_obj_add_state(child, LV_STATE_PRESSED);
        } else {
            lv_obj_remove_state(child, LV_STATE_PRESSED);
        }
        for (std::uint32_t j = 0; j < lv_obj_get_child_count(child); ++j) {
            lv_obj_t *part = lv_obj_get_child(child, j);
            if (pressed) {
                lv_obj_add_state(part, LV_STATE_PRESSED);
            } else {
                lv_obj_remove_state(part, LV_STATE_PRESSED);
            }
        }
    }
}

inline void style_button(lv_obj_t *button, std::uint32_t colour = panel_light)
{
    style_panel(button, colour, 14);
    fill_dim_accent(button, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(text), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, hand_down_press, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(button, hand_down_press, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(button, hand_down_press, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(button, 0, 0);
    lv_obj_set_style_outline_width(button, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(disabled), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_DISABLED);
    lv_obj_set_style_shadow_width(button, 0, LV_STATE_DISABLED);
    lv_obj_set_style_border_width(button, 0, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(button, lv_color_hex(disabled_ink), LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(button, LV_OPA_COVER, LV_STATE_DISABLED);
}

inline lv_obj_t *make_label(lv_obj_t *parent, const char *value, std::uint32_t colour = text,
                            const lv_font_t *font = nullptr)
{
    lv_obj_t *label = lv_label_create(parent);
    set_text(label, value);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(text), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(label, font != nullptr ? font : fonts::size_20(), 0);
    return label;
}

inline lv_obj_t *make_accent_label(lv_obj_t *parent, const char *value,
                                   const lv_font_t *font = nullptr)
{
    lv_obj_t *label = make_label(parent, value, text, font);
    ink_accent(label);
    return label;
}

inline lv_obj_t *make_button(lv_obj_t *parent, const char *value,
                             std::uint32_t colour  = panel_light,
                             const lv_font_t *font = nullptr)
{
    lv_obj_t *button = lv_button_create(parent);
    style_button(button, colour);
    lv_obj_center(make_label(button, value, text, font != nullptr ? font : fonts::size_28()));
    return button;
}

inline lv_obj_t *make_page_title(lv_obj_t *parent, const char *value)
{
    lv_obj_t *label = make_label(parent, value, text, &lv_font_montserrat_32);
    lv_obj_set_pos(label, 32, 22);
    return label;
}

inline lv_obj_t *make_card_heading(lv_obj_t *parent, const char *value)
{
    lv_obj_t *label = make_accent_label(parent, value, &lv_font_montserrat_16);
    lv_obj_set_pos(label, 20, 16);
    return label;
}

}  // namespace ui::theme
