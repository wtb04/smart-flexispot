#pragma once

#include "lvgl.h"
#include "units_font.h"

#include <cstdint>
#include <cstring>

// Orange on near-black, carried over from the earlier panel.
namespace ui::theme {

inline constexpr std::uint32_t background  = 0x15110f;
inline constexpr std::uint32_t panel       = 0x241c18;
inline constexpr std::uint32_t panel_light = 0x382820;
// A disabled control still has to read as a control, so it cannot take the background colour.
inline constexpr std::uint32_t disabled     = 0x1b1512;
inline constexpr std::uint32_t disabled_ink = 0x5c4d43;
inline constexpr std::uint32_t text        = 0xfff3ea;
inline constexpr std::uint32_t secondary   = 0xb7a092;
inline constexpr std::uint32_t orange      = 0xff8738;
// For marks drawn on top of an orange fill, where orange itself would vanish.
inline constexpr std::uint32_t orange_dim  = 0xc96a24;
inline constexpr std::uint32_t green       = 0x8bcb78;
inline constexpr std::uint32_t amber       = 0xffc15a;
inline constexpr std::uint32_t red         = 0xff6666;

// lv_label_set_text invalidates whether or not the text changed, and these are driven from
// timers, so a screen that is not changing would cost the renderer a repaint per tick.
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

// Same for styles: a write marks the object dirty even when the value is identical.
// Against the object's own default-state value, not the one its current state
// resolves to. Buttons and their labels carry pressed and disabled variants,
// so comparing the resolved colour skipped the write whenever the target
// happened to equal the state colour -- and the default was then never
// updated, so the control snapped back to the old colour when the state
// cleared. That is why a desk icon stayed orange on a highlighted preset.
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

// Without this the panel repainted tiles on every sensor report.
inline void set_bg_color(lv_obj_t *obj, std::uint32_t colour, lv_part_t part = LV_PART_MAIN)
{
    if (obj == nullptr) {
        return;
    }
    const lv_color_t next = lv_color_hex(colour);
    if (has_local_color(obj, LV_STYLE_BG_COLOR, part, next)) {
        return;
    }
    lv_obj_set_style_bg_color(obj, next, part);
}

inline void set_arc_color(lv_obj_t *obj, std::uint32_t colour, lv_part_t part)
{
    if (obj == nullptr) {
        return;
    }
    const lv_color_t next = lv_color_hex(colour);
    if (has_local_color(obj, LV_STYLE_ARC_COLOR, part, next)) {
        return;
    }
    lv_obj_set_style_arc_color(obj, next, part);
}

// lv_obj_align always writes the align style, and that invalidates the object and dirties its
// parent's layout -- unlike set_pos, set_width and set_height, which all compare first.
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

// LVGL does not hand a button's pressed state to its children, so a label with its own ink sits
// unchanged on a fill that just lit up. Two levels deep covers an icon and the parts it is built from.
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
    // Darker than an active control's own colour, so holding something already on still changes.
    lv_obj_set_style_bg_color(button, lv_color_hex(orange_dim), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(text), LV_STATE_PRESSED);
    lv_obj_add_event_cb(button, hand_down_press, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(button, hand_down_press, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(button, hand_down_press, LV_EVENT_PRESS_LOST, nullptr);
    // LVGL's default button style carries a shadow and an outline, which read as embossed.
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(button, 0, 0);
    lv_obj_set_style_outline_width(button, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(button, 0, LV_STATE_PRESSED);
    // Pressed state changes colour only, with no shift in geometry.
    lv_obj_set_style_translate_y(button, 0, LV_STATE_PRESSED);
    // Disabled is a real LVGL state with its own theme styling, which is how a dimmed button
    // ended up with no background at all.
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
    // Only ever reached inside a button, and only because style_button hands its state down.
    lv_obj_set_style_text_color(label, lv_color_hex(text), LV_STATE_PRESSED);
    lv_obj_set_style_text_font(label, font != nullptr ? font : fonts::size_20(), 0);
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
    lv_obj_t *label = make_label(parent, value, orange, &lv_font_montserrat_16);
    lv_obj_set_pos(label, 20, 16);
    return label;
}

}  // namespace ui::theme
