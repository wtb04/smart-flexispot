#pragma once

#include "icons.h"
#include "lvgl.h"
#include "units_font.h"

#include <array>
#include <algorithm>
#include <climits>
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

// ---------------------------------------------------------------------------
// The design language. Every page is built from these, so a page that looks
// different from the others is using a number that is not one of them.
//
// Surfaces, measured off the pages that already work: the screen is
// `background`, every page sits in a content area of `panel`, and cards on it
// are `panel_light`. What to look at first is found by type, not colour: the one
// display-size number on a page. A full accent fill means one thing only, that
// something is on, and accent text is for a selection, never for emphasis.
// Nothing is boxed inside anything else; within a card, space and type do the
// grouping. Labels are in sentence case and sit on what they name, not in a
// legend.

namespace space {
inline constexpr std::int32_t xs = 4;
inline constexpr std::int32_t s  = 8;
inline constexpr std::int32_t m  = 16;   // between cards
inline constexpr std::int32_t l  = 24;   // inside a card
inline constexpr std::int32_t xl = 32;
}  // namespace space

namespace radius {
inline constexpr std::int32_t card    = 16;
inline constexpr std::int32_t control = 14;
inline constexpr std::int32_t row     = 12;
inline constexpr std::int32_t pill    = LV_RADIUS_CIRCLE;
}  // namespace radius

// The round dots in a card's corners -- the chips that do something and the
// screws that do not -- are one thing: one size, set in by one amount.
namespace chip {
inline constexpr std::int32_t size  = 60;
inline constexpr std::int32_t inset = space::m;
}  // namespace chip

// Type roles rather than sizes, so a size means one thing everywhere.
inline const lv_font_t *type_display() { return fonts::size_48(); }  // the one big number
inline const lv_font_t *type_title() { return fonts::size_28(); }    // what a card is about
inline const lv_font_t *type_value() { return fonts::size_22(); }    // a reading
inline const lv_font_t *type_body() { return fonts::size_20(); }     // everything else
inline const lv_font_t *type_label() { return fonts::size_16(); }    // eyebrows and names

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

/** A card: the one surface content sits on. */
inline lv_obj_t *make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    style_panel(card, panel_light, radius::card);
    lv_obj_set_style_pad_all(card, space::l, 0);
    return card;
}

/** The small capitals over a reading or a block: CURRENT, LIGHTS, NEXT. */
inline lv_obj_t *make_eyebrow(lv_obj_t *parent, const char *value,
                              std::uint32_t colour = secondary)
{
    lv_obj_t *label = make_label(parent, value, colour, type_label());
    lv_obj_set_style_text_letter_space(label, 1, 0);
    return label;
}

/** A name on the left and a reading on the right, as the radar lists them. */
struct Stat {
    lv_obj_t *row   = nullptr;
    lv_obj_t *name  = nullptr;
    lv_obj_t *value = nullptr;
};

inline Stat make_stat(lv_obj_t *parent, const char *name)
{
    Stat stat;
    stat.row = lv_obj_create(parent);
    style_panel(stat.row, panel, 0);
    lv_obj_set_style_bg_opa(stat.row, LV_OPA_TRANSP, 0);
    lv_obj_set_width(stat.row, LV_PCT(100));
    lv_obj_set_height(stat.row, type_value()->line_height);
    lv_obj_set_flex_flow(stat.row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stat.row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stat.row, space::m, 0);
    lv_obj_set_clickable(stat.row, false);

    stat.name  = make_label(stat.row, name, secondary, type_label());
    stat.value = make_label(stat.row, "--", text, type_value());
    lv_obj_set_flex_grow(stat.value, 1);
    lv_obj_set_style_text_align(stat.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(stat.value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(stat.value, type_value()->line_height);
    return stat;
}

/** A Pozidriv screw head: the cross, and a finer one turned between its arms.
 *  Decoration that says a card is an instrument; it does nothing, so it is the
 *  quietest thing on it. The chips that do something are the same dot. */
/** Centres a short label by the ink of its glyphs rather than its line box,
 *  which leaves a plus or a digit visibly off the middle of a small chip. */
inline void center_ink(lv_obj_t *label)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const char      *text = lv_label_get_text(label);
    const std::int32_t ascent = font->line_height - font->base_line;

    std::int32_t pen = 0;
    std::int32_t left = INT32_MAX, right = INT32_MIN, top = INT32_MAX, bottom = INT32_MIN;
    for (std::uint32_t i = 0; text[i] != '\0';) {
        // UTF-8 by hand: LVGL keeps its decoder in a private header.
        const auto    lead   = static_cast<unsigned char>(text[i]);
        const int     extra  = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : 0;
        std::uint32_t letter = extra == 0 ? lead : lead & (0x3f >> extra);
        ++i;
        for (int k = 0; k < extra && text[i] != '\0'; ++k, ++i) {
            letter = (letter << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        }
        lv_font_glyph_dsc_t glyph;
        if (!lv_font_get_glyph_dsc(font, &glyph, letter, 0)) {
            continue;
        }
        if (glyph.box_w > 0 && glyph.box_h > 0) {
            const std::int32_t glyph_top = ascent - (glyph.ofs_y + glyph.box_h);
            left   = std::min<std::int32_t>(left, pen + glyph.ofs_x);
            right  = std::max<std::int32_t>(right, pen + glyph.ofs_x + glyph.box_w);
            top    = std::min(top, glyph_top);
            bottom = std::max<std::int32_t>(bottom, glyph_top + glyph.box_h);
        }
        pen += glyph.adv_w;
    }
    if (left > right) {
        lv_obj_center(label);
        return;
    }
    lv_obj_align(label, LV_ALIGN_CENTER, pen / 2 - (left + right) / 2,
                 font->line_height / 2 - (top + bottom) / 2);
}

inline constexpr lv_opa_t mark_opa = LV_OPA_60;

inline lv_obj_t *make_mark(lv_obj_t *parent, const lv_image_dsc_t *mark, lv_opa_t opa = mark_opa)
{
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, mark);
    lv_obj_set_style_image_recolor(image, lv_color_hex(secondary), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(image, opa, 0);
    lv_obj_center(image);
    lv_obj_set_clickable(image, false);
    return image;
}

inline lv_obj_t *make_screw(lv_obj_t *parent, std::int32_t size)
{
    lv_obj_t *head = lv_obj_create(parent);
    lv_obj_set_size(head, size, size);
    style_panel(head, panel, size / 2);
    lv_obj_set_clickable(head, false);
    // A plus with a smaller times sign over it: a Pozidriv head.
    make_mark(head, &icons::plus_icon);
    make_mark(head, &icons::times_icon, LV_OPA_50);
    return head;
}

/** A chip: a round button in a card's corner. The glyph is the screws' grey,
 *  so a row of chips and screws reads as one set, and lights when pressed. */
/** The default font's digits stand as tall as the drawn plus on the screws. */
inline lv_obj_t *make_chip(lv_obj_t *parent, const char *value,
                           const lv_font_t *font = type_body())
{
    lv_obj_t *chip = make_button(parent, value, panel, font);
    lv_obj_set_size(chip, chip::size, chip::size);
    lv_obj_set_style_radius(chip, chip::size / 2, 0);
    lv_obj_t *label = lv_obj_get_child(chip, 0);
    set_text_color(label, secondary);
    lv_obj_set_style_text_opa(label, mark_opa, 0);
    center_ink(label);
    return chip;
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
