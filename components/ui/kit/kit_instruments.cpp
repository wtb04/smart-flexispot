#include "kit.h"

#include <algorithm>
#include <cmath>

namespace ui::kit {
namespace {

constexpr float DEG = 3.14159265f / 180.0f;

void animate(void *var, void *owner, std::int32_t from, std::int32_t to, lv_anim_custom_exec_cb_t exec)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_user_data(&a, owner);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_custom_exec_cb(&a, exec);
    lv_anim_set_duration(&a, motion::move_ms);
    lv_anim_set_path_cb(&a, motion::spring);
    lv_anim_start(&a);
}

}  // namespace

Dial::Dial(lv_obj_t *parent, const DialSpec &spec) : m_spec(spec)
{
    m_root = lv_obj_create(parent);
    make_arc();
}

float Dial::angle_of(std::int32_t value) const
{
    const std::int32_t v = std::clamp(value, m_spec.min, m_spec.max);
    return (static_cast<float>(m_spec.start) +
            static_cast<float>(m_spec.sweep) * static_cast<float>(v - m_spec.min) /
                static_cast<float>(m_spec.max - m_spec.min)) * DEG;
}

void Dial::set_value(std::int32_t value, bool)
{
    const bool none = value == INT32_MIN;
    if (!none) {
        m_value = std::clamp(value, m_spec.min, m_spec.max);
        lv_arc_set_value(m_arc, m_value);  // sets nothing when it is there already
    }
    if (none != m_none) {
        m_none = none;
        restyle_arc();
    }
}

void Dial::make_arc()
{
    lv_obj_remove_style_all(m_root);
    lv_obj_set_scrollable(m_root, false);
    lv_obj_set_clickable(m_root, false);
    const std::int32_t W = m_spec.arc;
    m_arc = lv_arc_create(m_root);
    lv_obj_remove_style_all(m_arc);
    lv_arc_set_rotation(m_arc, static_cast<std::uint32_t>(m_spec.start));
    lv_arc_set_bg_angles(m_arc, 0, static_cast<std::uint32_t>(m_spec.sweep));
    lv_arc_set_mode(m_arc, m_spec.reverse ? LV_ARC_MODE_REVERSE : LV_ARC_MODE_NORMAL);
    lv_arc_set_range(m_arc, m_spec.min, m_spec.max);
    for (lv_part_t part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
        lv_obj_set_style_arc_width(m_arc, W, part);
        lv_obj_set_style_arc_rounded(m_arc, true, part);
    }
    // the knob stays inside the box: outside it would be cut off
    const std::int32_t knob = m_spec.target ? W / 4 + 2 : 0;
    lv_obj_set_style_pad_all(m_arc, knob, LV_PART_MAIN);
    m_r_arc = knob + W / 2;  // in from the box's edge to the middle of the arc
    if (m_spec.target) {
        lv_obj_set_style_bg_opa(m_arc, LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_radius(m_arc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
        lv_obj_set_style_pad_all(m_arc, knob, LV_PART_KNOB);
    }
    if (m_spec.drag) {
        lv_obj_set_clickable(m_arc, true);
        lv_obj_set_ext_click_area(m_arc, W);
        lv_obj_add_event_cb(
            m_arc,
            [](lv_event_t *e) {
                auto *d    = static_cast<Dial *>(lv_event_get_user_data(e));
                d->m_value = lv_arc_get_value(d->m_arc);
                lv_obj_send_event(d->m_root, LV_EVENT_VALUE_CHANGED, nullptr);
            },
            LV_EVENT_VALUE_CHANGED, this);
        for (lv_event_code_t code : {LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
            lv_obj_add_event_cb(
                m_arc,
                [](lv_event_t *e) {
                    lv_obj_send_event(static_cast<Dial *>(lv_event_get_user_data(e))->m_root, LV_EVENT_RELEASED, nullptr);
                },
                code, this);
        }
    } else {
        lv_obj_set_clickable(m_arc, false);
    }
    if (m_spec.mark) {
        m_dot = lv_obj_create(m_root);
        lv_obj_remove_style_all(m_dot);
        const std::int32_t d = std::max<std::int32_t>(6, W * 2 / 5);
        lv_obj_set_size(m_dot, d, d);
        lv_obj_set_style_radius(m_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(m_dot, LV_OPA_COVER, 0);
        lv_obj_set_ignore_layout(m_dot, true);
        lv_obj_set_clickable(m_dot, false);
        lv_obj_set_hidden(m_dot, true);
    }

    lv_obj_t *mid = lv_obj_create(m_root);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(mid, false);
    lv_obj_set_scrollable(mid, false);
    lv_obj_center(mid);
    if (m_spec.over) {
        m_over = text(mid, Role::Small, "", Ink::Secondary);
        lv_obj_set_style_text_align(m_over, LV_TEXT_ALIGN_CENTER, 0);
    }
    m_read = text(mid, m_spec.read, "");
    lv_obj_set_style_text_align(m_read, LV_TEXT_ALIGN_CENTER, 0);
    if (m_spec.unit != nullptr) {
        m_unit = text(mid, Role::Small, m_spec.unit, Ink::Secondary);
        lv_obj_set_style_text_align(m_unit, LV_TEXT_ALIGN_CENTER, 0);
    }
    m_value = m_spec.min;
    lv_arc_set_value(m_arc, m_spec.min);
    set_size(m_spec.size);
    restyle_arc();
    on_restyle(m_root, [](void *ctx) { static_cast<Dial *>(ctx)->restyle_arc(); }, this);
}

void Dial::restyle_arc()
{
    const lv_color_t ink = colour(m_ink);
    lv_obj_set_style_arc_color(m_arc, colour(Ink::Primary), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(m_arc, static_cast<lv_opa_t>(colours().fill_opa * 2), LV_PART_MAIN);
    lv_obj_set_style_arc_color(m_arc, ink, LV_PART_INDICATOR);
    const lv_opa_t opa = m_none ? LV_OPA_TRANSP : m_faded ? LV_OPA_40 : LV_OPA_COVER;
    lv_obj_set_style_arc_opa(m_arc, opa, LV_PART_INDICATOR);
    if (m_spec.target) {
        lv_obj_set_style_bg_color(m_arc, lv_color_hex(0xffffff), LV_PART_KNOB);
        lv_obj_set_style_bg_opa(m_arc, m_none ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_KNOB);
        lv_obj_set_style_border_color(m_arc, ink, LV_PART_KNOB);
        lv_obj_set_style_border_width(m_arc, std::max<std::int32_t>(3, m_spec.arc / 7), LV_PART_KNOB);
        lv_obj_set_style_border_opa(m_arc, opa, LV_PART_KNOB);
    }
    if (m_dot != nullptr) {
        lv_obj_set_style_bg_color(m_dot, colour(Ink::Primary), 0);
    }
}

void Dial::set_ink(Ink ink)
{
    if (ink != m_ink) {
        m_ink = ink;
        restyle_arc();
    }
}

void Dial::set_faded(bool faded)
{
    if (faded != m_faded) {
        m_faded = faded;
        restyle_arc();
    }
}

void Dial::set_range(std::int32_t min, std::int32_t max)
{
    if (max <= min || (min == m_spec.min && max == m_spec.max)) {
        return;
    }
    m_spec.min = min;
    m_spec.max = max;
    lv_arc_set_range(m_arc, min, max);
    set_value(m_value);
    place_mark();
}

void Dial::set_mark(std::int32_t value)
{
    if (value != m_mark_at) {
        m_mark_at = value;
        place_mark();
    }
}

void Dial::place_mark()
{
    if (m_dot == nullptr) {
        return;
    }
    const bool none = m_mark_at == INT32_MIN;
    if (lv_obj_is_hidden(m_dot) != none) {
        lv_obj_set_hidden(m_dot, none);
    }
    if (none) {
        return;
    }
    const float r = m_spec.size / 2.0f - static_cast<float>(m_r_arc), a = angle_of(m_mark_at), c = m_spec.size / 2.0f;
    const std::int32_t d = lv_obj_get_style_width(m_dot, LV_PART_MAIN);
    const auto x = static_cast<std::int32_t>(std::lround(c + r * std::cos(a))) - d / 2;
    const auto y = static_cast<std::int32_t>(std::lround(c + r * std::sin(a))) - d / 2;
    if (lv_obj_get_style_x(m_dot, LV_PART_MAIN) != x || lv_obj_get_style_y(m_dot, LV_PART_MAIN) != y) {
        lv_obj_set_pos(m_dot, x, y);
    }
}

void Dial::set_size(std::int32_t size, std::int32_t arc)
{
    m_spec.size = size;
    if (arc > 0 && arc != m_spec.arc) {
        m_spec.arc = arc;
        const std::int32_t knob = m_spec.target ? arc / 4 + 2 : 0;
        for (lv_part_t part : {LV_PART_MAIN, LV_PART_INDICATOR}) {
            lv_obj_set_style_arc_width(m_arc, arc, part);
        }
        lv_obj_set_style_pad_all(m_arc, knob, LV_PART_MAIN);
        lv_obj_set_style_pad_all(m_arc, knob, LV_PART_KNOB);
        m_r_arc = knob + arc / 2;
    }
    if (lv_obj_get_style_width(m_root, LV_PART_MAIN) != size) {
        lv_obj_set_size(m_root, size, size);
        lv_obj_set_size(m_arc, size, size);
    }
    place_mark();
}

// --- rails ----------------------------------------------------------------

namespace {
constexpr std::int32_t TRACK = 12, KNOB = 28;
constexpr std::int32_t END = (KNOB - TRACK) / 2;  // the track stops short by this, so the knob at either end stays on the rail
}

Rail::Rail(lv_obj_t *parent, const RailSpec &spec) : m_spec(spec)
{
    const bool v = spec.vertical;
    const std::int32_t cross = spec.ticks > 0 ? KNOB + 18 : KNOB;
    m_root = lv_obj_create(parent);
    lv_obj_remove_style_all(m_root);
    lv_obj_set_size(m_root, v ? cross : spec.length, v ? spec.length : cross);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m_root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // the knob's shadow
    const std::int32_t len = spec.length - 2 * END;

    m_track = well(m_root);
    lv_obj_set_style_radius(m_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(m_track, 0, 0);
    lv_obj_set_size(m_track, v ? TRACK : len, v ? len : TRACK);
    lv_obj_set_pos(m_track, END, END);
    lv_obj_remove_flag(m_track, LV_OBJ_FLAG_CLICKABLE);
    if (spec.quiet) {
        constexpr std::int32_t LINE = 4;
        lv_obj_set_size(m_track, v ? LINE : len, v ? len : LINE);
        lv_obj_set_pos(m_track, v ? END + (TRACK - LINE) / 2 : END, v ? END : END + (TRACK - LINE) / 2);
    }

    m_fill = lv_obj_create(m_track);
    lv_obj_remove_style_all(m_fill);
    lv_obj_set_style_radius(m_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m_fill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(m_fill, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < spec.notch_count; ++i) {
        lv_obj_t *n = lv_obj_create(m_track);
        lv_obj_remove_style_all(n);
        lv_obj_set_size(n, 6, 6);
        lv_obj_set_style_radius(n, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(n, LV_OBJ_FLAG_USER_1);
        const std::int32_t at = (spec.notches[i] - spec.min) * (len - TRACK) / (spec.max - spec.min);
        lv_obj_set_pos(n, v ? 2 : at + 3, v ? len - TRACK - at + 3 : 2);
        lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
    }
    if (spec.ticks > 0) {
        lv_obj_t *sc = lv_scale_create(m_root);
        lv_scale_set_mode(sc, v ? LV_SCALE_MODE_VERTICAL_RIGHT : LV_SCALE_MODE_HORIZONTAL_BOTTOM);
        lv_scale_set_range(sc, spec.min, spec.max);
        lv_scale_set_total_tick_count(sc, static_cast<std::uint32_t>(spec.ticks));
        lv_scale_set_major_tick_every(sc, static_cast<std::uint32_t>(spec.major));
        lv_scale_set_label_show(sc, false);
        lv_obj_set_size(sc, v ? 14 : len - TRACK, v ? len - TRACK : 14);
        lv_obj_set_pos(sc, v ? KNOB + 2 : END + TRACK / 2, v ? END + TRACK / 2 : KNOB + 2);
        lv_obj_set_style_line_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_add_flag(sc, LV_OBJ_FLAG_USER_2);
        for (lv_part_t part : {LV_PART_INDICATOR, LV_PART_ITEMS}) {
            lv_obj_set_style_line_width(sc, part == LV_PART_INDICATOR ? 2 : 1, part);
        }
        lv_obj_set_style_length(sc, 9, LV_PART_INDICATOR);
        lv_obj_set_style_length(sc, 5, LV_PART_ITEMS);
        if (v) {
            lv_scale_set_range(sc, spec.max, spec.min);
        }
    }

    m_knob = lv_obj_create(m_root);
    lv_obj_remove_style_all(m_knob);
    lv_obj_set_size(m_knob, KNOB, KNOB);
    lv_obj_set_style_radius(m_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(m_knob, 12, 0);
    lv_obj_set_style_shadow_offset_y(m_knob, 3, 0);
    lv_obj_set_style_shadow_opa(m_knob, 120, 0);
    lv_obj_set_style_transform_pivot_x(m_knob, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(m_knob, lv_pct(50), 0);
    lv_obj_remove_flag(m_knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_hidden(m_knob, spec.quiet);

    if (spec.draggable) {
        lv_obj_add_flag(m_root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(m_root, 16);
        lv_obj_add_event_cb(m_root, on_press, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(m_root, on_press, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(m_root, on_press, LV_EVENT_RELEASED, this);
    }
    restyle();
    on_restyle(m_root, [](void *ctx) { static_cast<Rail *>(ctx)->restyle(); }, this);
    m_value = m_shown = spec.min;
    place(spec.min);
}

void Rail::restyle()
{
    lv_obj_set_style_bg_color(m_fill, colour(m_spec.quiet ? Ink::Primary : Ink::Accent), 0);
    lv_obj_set_style_bg_color(m_knob, lv_color_hex(0xffffff), 0);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(m_track); ++i) {
        lv_obj_t *n = lv_obj_get_child(m_track, static_cast<int32_t>(i));
        if (lv_obj_has_flag(n, LV_OBJ_FLAG_USER_1)) {
            lv_obj_set_style_bg_color(n, colour(Ink::Primary), 0);
            lv_obj_set_style_bg_opa(n, colours().ink3_opa, 0);
        }
    }
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(m_root); ++i) {
        lv_obj_t *sc = lv_obj_get_child(m_root, static_cast<int32_t>(i));
        if (lv_obj_has_flag(sc, LV_OBJ_FLAG_USER_2)) {
            for (lv_part_t part : {LV_PART_INDICATOR, LV_PART_ITEMS}) {
                lv_obj_set_style_line_color(sc, colour(Ink::Primary), part);
                lv_obj_set_style_line_opa(sc, colours().tick_opa, part);
            }
        }
    }
}

void Rail::place(std::int32_t value)
{
    m_shown = value;
    const bool v = m_spec.vertical;
    const std::int32_t len = m_spec.length - 2 * END, span = len - TRACK;
    const std::int32_t at = (std::clamp(value, m_spec.min, m_spec.max) - m_spec.min) * span / (m_spec.max - m_spec.min);
    const auto put = [](lv_obj_t *obj, std::int32_t x, std::int32_t y) {
        if (lv_obj_get_style_x(obj, LV_PART_MAIN) != x || lv_obj_get_style_y(obj, LV_PART_MAIN) != y) {
            lv_obj_set_pos(obj, x, y);
        }
    };
    const std::int32_t thick = m_spec.quiet ? 4 : TRACK;  // a quiet track's line, which would cut a thicker fill in half
    const std::int32_t w = v ? thick : at + TRACK, h = v ? at + TRACK : thick;
    if (lv_obj_get_style_width(m_fill, LV_PART_MAIN) != w || lv_obj_get_style_height(m_fill, LV_PART_MAIN) != h) {
        lv_obj_set_size(m_fill, w, h);
    }
    put(m_fill, 0, v ? len - at - TRACK : 0);
    const std::int32_t knob_at = at + TRACK / 2 - KNOB / 2;
    put(m_knob, v ? 0 : END + knob_at, v ? END + len - knob_at - KNOB : 0);
}

void Rail::on_value(void *var, int32_t v) { static_cast<Rail *>(static_cast<lv_anim_t *>(var)->user_data)->place(v); }

void Rail::set_value(std::int32_t value, bool animate_it)
{
    const std::int32_t from = m_shown;
    m_value = std::clamp(value, m_spec.min, m_spec.max);
    lv_anim_delete(&m_shown, nullptr);
    if (!animate_it || from == m_value) {
        place(m_value);
        return;
    }
    animate(&m_shown, this, from, m_value, [](lv_anim_t *a, int32_t v) { on_value(a, v); });
}

void Rail::on_press(lv_event_t *e)
{
    auto *rail = static_cast<Rail *>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    const auto scale = [](void *knob, int32_t v) {
        lv_obj_set_style_transform_scale_x(static_cast<lv_obj_t *>(knob), v, 0);
        lv_obj_set_style_transform_scale_y(static_cast<lv_obj_t *>(knob), v, 0);
    };
    // grabbed at once, let go on the spring
    if (rail->m_spec.quiet) {
        lv_obj_set_hidden(rail->m_knob, code == LV_EVENT_RELEASED);
    }
    if (code == LV_EVENT_RELEASED) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, rail->m_knob);
        lv_anim_set_values(&a, motion::grab_scale, 256);
        lv_anim_set_exec_cb(&a, scale);
        lv_anim_set_duration(&a, motion::release_ms);
        lv_anim_set_path_cb(&a, motion::spring);
        lv_anim_start(&a);
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        lv_anim_delete(rail->m_knob, scale);
        scale(rail->m_knob, motion::grab_scale);
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t box;
    lv_obj_get_coords(rail->m_track, &box);
    const bool v = rail->m_spec.vertical;
    const std::int32_t span = rail->m_spec.length - 2 * END - TRACK;
    const std::int32_t pos = v ? box.y2 - TRACK / 2 - p.y : p.x - box.x1 - TRACK / 2;
    const std::int32_t value = rail->m_spec.min + std::clamp<std::int32_t>(pos, 0, span) * (rail->m_spec.max - rail->m_spec.min) / span;
    if (code == LV_EVENT_PRESSING && value == rail->m_value) {
        return;  // a finger held still: nothing moved, nothing to tell
    }
    rail->set_value(value, code == LV_EVENT_PRESSED);
    lv_obj_send_event(rail->m_root, LV_EVENT_VALUE_CHANGED, nullptr);
}

}  // namespace ui::kit
