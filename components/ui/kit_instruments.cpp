#include "kit.h"

#include "kit_fonts.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui::kit {
namespace {

constexpr float DEG = 3.14159265f / 180.0f;

void format(char *out, std::size_t n, std::int32_t value, int decimals)
{
    if (decimals == 1) {
        const std::int32_t whole = value / 10, tenth = std::abs(value % 10);
        std::snprintf(out, n, "%s%d.%d", value < 0 && whole == 0 ? "-" : "", static_cast<int>(whole),
                      static_cast<int>(tenth));
    } else {
        std::snprintf(out, n, "%d", static_cast<int>(value));
    }
}

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
    const std::int32_t S = spec.size, band = std::max<std::int32_t>(3, S * 18 / 1000);
    m_major_len = std::max<std::int32_t>(7, S * 55 / 1000);
    const std::int32_t minor_len = std::max<std::int32_t>(4, S * 28 / 1000);
    m_root = lv_obj_create(parent);
    lv_obj_remove_style_all(m_root);
    lv_obj_set_size(m_root, S, S);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_CLICKABLE);

    m_scale = lv_scale_create(m_root);
    lv_obj_set_size(m_scale, S, S);
    lv_obj_center(m_scale);
    lv_scale_set_mode(m_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(m_scale, spec.min, spec.max);
    lv_scale_set_total_tick_count(m_scale, static_cast<std::uint32_t>(spec.ticks));
    lv_scale_set_major_tick_every(m_scale, static_cast<std::uint32_t>(spec.major));
    lv_scale_set_angle_range(m_scale, static_cast<std::uint32_t>(spec.sweep));
    lv_scale_set_rotation(m_scale, spec.start);
    lv_obj_set_style_arc_width(m_scale, band, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(m_scale, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(m_scale, false, LV_PART_MAIN);
    lv_obj_set_style_pad_all(m_scale, 0, LV_PART_MAIN);
    lv_obj_set_style_radial_offset(m_scale, -S * 2 / 100, LV_PART_INDICATOR);
    lv_obj_set_style_radial_offset(m_scale, -S * 2 / 100, LV_PART_ITEMS);
    lv_obj_set_style_length(m_scale, m_major_len, LV_PART_INDICATOR);
    lv_obj_set_style_length(m_scale, minor_len, LV_PART_ITEMS);
    lv_obj_set_style_line_width(m_scale, std::max<std::int32_t>(2, S / 110), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(m_scale, std::max<std::int32_t>(1, S / 220), LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(m_scale, true, LV_PART_INDICATOR);
    if (spec.ends) {
        format(m_ends[0], sizeof m_ends[0], spec.min, spec.decimals);
        format(m_ends[1], sizeof m_ends[1], spec.max, spec.decimals);
        const int majors = (spec.ticks - 1) / spec.major + 1;
        for (int i = 0; i < majors && i < 70; ++i) {
            m_src[i] = i == 0 ? m_ends[0] : i == majors - 1 ? m_ends[1] : "";
        }
        m_src[std::min(majors, 70)] = nullptr;
        lv_scale_set_text_src(m_scale, m_src);
        lv_scale_set_label_show(m_scale, true);
        lv_obj_set_style_text_font(m_scale, fonts::label(), LV_PART_INDICATOR);
        lv_obj_set_style_pad_radial(m_scale, 8, LV_PART_INDICATOR);
    } else {
        lv_scale_set_label_show(m_scale, false);
    }

    for (int i = 0; i < spec.zone_count && i < 6; ++i) {
        lv_style_init(&m_zone_styles[i]);
        lv_style_set_arc_width(&m_zone_styles[i], band);
        lv_style_set_arc_opa(&m_zone_styles[i], 70);
        m_zones[i] = lv_scale_add_section(m_scale);
        lv_scale_section_set_range(m_zones[i], spec.zones[i].from, spec.zones[i].to);
        lv_scale_section_set_style(m_zones[i], LV_PART_MAIN, &m_zone_styles[i]);
    }

    m_r_tick = S / 2 - band - S * 2 / 100;
    const std::int32_t arc_w = std::max<std::int32_t>(5, S * 45 / 1000);
    if (spec.fill) {
        const std::int32_t inset = band + m_major_len + S * 6 / 100;
        m_arc = lv_arc_create(m_root);
        m_r_arc = (S - 2 * inset) / 2 - arc_w / 2;
        lv_obj_set_size(m_arc, S - 2 * inset, S - 2 * inset);
        lv_obj_center(m_arc);
        lv_arc_set_rotation(m_arc, static_cast<std::uint32_t>(spec.start));
        lv_arc_set_bg_angles(m_arc, 0, static_cast<std::uint32_t>(spec.sweep));
        lv_arc_set_range(m_arc, spec.min, spec.max);
        lv_obj_remove_flag(m_arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(m_arc, arc_w, LV_PART_MAIN);
        lv_obj_set_style_arc_width(m_arc, arc_w, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(m_arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(m_arc, 0, LV_PART_KNOB);
        lv_obj_set_style_pad_all(m_arc, 0, LV_PART_MAIN);
    }
    if (spec.target) {
        m_knob = lv_obj_create(m_root);
        lv_obj_remove_style_all(m_knob);
        lv_obj_set_size(m_knob, arc_w * 2 + 4, arc_w * 2 + 4);
        lv_obj_set_style_radius(m_knob, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(m_knob, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(m_knob, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_width(m_knob, 10, 0);
        lv_obj_set_style_shadow_offset_y(m_knob, 2, 0);
        lv_obj_set_style_shadow_opa(m_knob, 110, 0);
        lv_obj_add_flag(m_knob, LV_OBJ_FLAG_IGNORE_LAYOUT);
    }
    if (spec.needle) {
        m_needle = lv_line_create(m_root);
        lv_obj_set_style_line_width(m_needle, std::max<std::int32_t>(3, S * 16 / 1000), 0);
        lv_obj_set_style_line_rounded(m_needle, true, 0);
        lv_obj_add_flag(m_needle, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_pos(m_needle, 0, 0);
        lv_obj_set_size(m_needle, S, S);
    }

    lv_obj_t *mid = lv_obj_create(m_root);
    lv_obj_remove_style_all(mid);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(mid);
    m_read = text(mid, spec.read, "");
    if (spec.unit != nullptr) {
        m_unit = text(mid, Role::Label, spec.unit, Ink::Secondary);
    }
    restyle();
    on_restyle(m_root, [](void *ctx) { static_cast<Dial *>(ctx)->restyle(); }, this);
    m_value = m_target = spec.min;
    set_value(spec.min, false);
    if (spec.target) {
        set_target(spec.min, false);
    }
}

void Dial::restyle()
{
    const lv_color_t ink = colour(Ink::Primary);
    for (lv_part_t part : {LV_PART_INDICATOR, LV_PART_ITEMS}) {
        lv_obj_set_style_line_color(m_scale, ink, part);
        lv_obj_set_style_line_opa(m_scale, colours().tick_opa, part);
    }
    lv_obj_set_style_text_color(m_scale, ink, LV_PART_INDICATOR);
    lv_obj_set_style_text_opa(m_scale, colours().ink3_opa, LV_PART_INDICATOR);
    for (int i = 0; i < m_spec.zone_count && i < 6; ++i) {
        lv_style_set_arc_color(&m_zone_styles[i], colour(m_spec.zones[i].ink));
    }
    if (m_lit_section != nullptr) {
        lv_style_set_line_color(&m_lit_style, colour(Ink::Accent));
    }
    if (m_arc != nullptr) {
        lv_obj_set_style_arc_color(m_arc, lv_color_hex(colours().well), LV_PART_MAIN);
        lv_obj_set_style_arc_color(m_arc, colour(Ink::Accent), LV_PART_INDICATOR);
    }
    if (m_needle != nullptr) {
        lv_obj_set_style_line_color(m_needle, ink, 0);
    }
    lv_obj_invalidate(m_root);
}

float Dial::angle_of(std::int32_t value) const
{
    const std::int32_t v = std::clamp(value, m_spec.min, m_spec.max);
    return (static_cast<float>(m_spec.start) +
            static_cast<float>(m_spec.sweep) * static_cast<float>(v - m_spec.min) /
                static_cast<float>(m_spec.max - m_spec.min)) * DEG;
}

void Dial::place_needle(std::int32_t value)
{
    if (m_needle == nullptr) {
        return;
    }
    const float a = angle_of(value), c = m_spec.size / 2.0f;
    const float r0 = static_cast<float>(m_r_tick) + 2.0f, r1 = static_cast<float>(m_r_tick - m_major_len * 17 / 10);
    m_points[0] = {static_cast<lv_value_precise_t>(c + r0 * std::cos(a)), static_cast<lv_value_precise_t>(c + r0 * std::sin(a))};
    m_points[1] = {static_cast<lv_value_precise_t>(c + r1 * std::cos(a)), static_cast<lv_value_precise_t>(c + r1 * std::sin(a))};
    lv_line_set_points(m_needle, m_points, 2);
}

void Dial::place_target(std::int32_t value)
{
    if (m_arc != nullptr) {
        lv_arc_set_value(m_arc, value);
    }
    if (m_knob != nullptr && m_arc != nullptr) {
        const float r = static_cast<float>(m_r_arc), a = angle_of(value), c = m_spec.size / 2.0f;
        const std::int32_t k = lv_obj_get_style_width(m_knob, LV_PART_MAIN);
        lv_obj_set_pos(m_knob, static_cast<std::int32_t>(c + r * std::cos(a)) - k / 2,
                       static_cast<std::int32_t>(c + r * std::sin(a)) - k / 2);
    }
}

void Dial::show_value(std::int32_t value)
{
    if (value == m_shown) {
        return;
    }
    m_shown = value;
    char buf[16];
    format(buf, sizeof buf, value, m_spec.decimals);
    lv_label_set_text(m_read, buf);
}

void Dial::light_zone(std::int32_t value)
{
    int zone = -1;
    for (int i = 0; i < m_spec.zone_count && i < 6; ++i) {
        if (value >= m_spec.zones[i].from && value <= m_spec.zones[i].to) {
            zone = i;
        }
    }
    if (zone == m_zone) {
        return;
    }
    m_zone = zone;
    for (int i = 0; i < m_spec.zone_count && i < 6; ++i) {
        lv_style_set_arc_opa(&m_zone_styles[i], i == zone ? LV_OPA_COVER : 70);
    }
    lv_obj_invalidate(m_scale);
}

void Dial::on_value(void *var, int32_t v)
{
    auto *d = static_cast<Dial *>(static_cast<lv_anim_t *>(var)->user_data);
    d->place_needle(v);
    d->show_value(v);
}

void Dial::on_target(void *var, int32_t v)
{
    auto *d = static_cast<Dial *>(static_cast<lv_anim_t *>(var)->user_data);
    d->place_target(v);
    if (!d->m_spec.needle) {
        d->show_value(v);
    }
}

void Dial::set_value(std::int32_t value, bool animate_it)
{
    const std::int32_t from = m_value;
    m_value = value;
    light_zone(value);
    lv_anim_delete(&m_value, nullptr);
    if (!animate_it || from == value) {
        place_needle(value);
        show_value(value);
        return;
    }
    animate(&m_value, this, from, value, [](lv_anim_t *a, int32_t v) { on_value(a, v); });
}

void Dial::set_target(std::int32_t value, bool animate_it)
{
    const std::int32_t from = m_target;
    m_target = value;
    lv_anim_delete(&m_target, nullptr);
    if (!animate_it || from == value) {
        place_target(value);
        if (!m_spec.needle) {
            show_value(value);
        }
        return;
    }
    animate(&m_target, this, from, value, [](lv_anim_t *a, int32_t v) { on_target(a, v); });
}

void Dial::set_lit(int ticks)
{
    if (ticks == m_lit) {
        return;
    }
    m_lit = ticks;
    if (m_lit_section == nullptr) {
        lv_style_init(&m_lit_style);
        lv_style_set_line_color(&m_lit_style, colour(Ink::Accent));
        lv_style_set_line_opa(&m_lit_style, LV_OPA_COVER);
        m_lit_section = lv_scale_add_section(m_scale);
        lv_scale_section_set_style(m_lit_section, LV_PART_INDICATOR, &m_lit_style);
        lv_scale_section_set_style(m_lit_section, LV_PART_ITEMS, &m_lit_style);
    }
    const std::int32_t step = (m_spec.max - m_spec.min) / std::max<std::int32_t>(1, m_spec.ticks - 1);
    lv_scale_section_set_range(m_lit_section, m_spec.min, ticks <= 0 ? m_spec.min - 1 : m_spec.min + step * (ticks - 1));
    lv_obj_invalidate(m_scale);
}

// --- rails ----------------------------------------------------------------

namespace {
constexpr std::int32_t TRACK = 12, KNOB = 28;
}

Rail::Rail(lv_obj_t *parent, const RailSpec &spec) : m_spec(spec)
{
    const bool v = spec.vertical;
    const std::int32_t cross = spec.ticks > 0 ? KNOB + 18 : KNOB;
    m_root = lv_obj_create(parent);
    lv_obj_remove_style_all(m_root);
    lv_obj_set_size(m_root, v ? cross : spec.length, v ? spec.length : cross);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_SCROLLABLE);

    m_track = well(m_root);
    lv_obj_set_style_radius(m_track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(m_track, 0, 0);
    lv_obj_set_size(m_track, v ? TRACK : spec.length, v ? spec.length : TRACK);
    lv_obj_set_pos(m_track, v ? (KNOB - TRACK) / 2 : 0, v ? 0 : (KNOB - TRACK) / 2);
    lv_obj_remove_flag(m_track, LV_OBJ_FLAG_CLICKABLE);

    if (spec.fill) {
        m_fill = lv_obj_create(m_track);
        lv_obj_remove_style_all(m_fill);
        lv_obj_set_style_radius(m_fill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(m_fill, LV_OPA_COVER, 0);
        lv_obj_remove_flag(m_fill, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < spec.notch_count; ++i) {
        lv_obj_t *n = lv_obj_create(m_track);
        lv_obj_remove_style_all(n);
        lv_obj_set_size(n, 6, 6);
        lv_obj_set_style_radius(n, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(n, LV_OBJ_FLAG_USER_1);
        const std::int32_t at = (spec.notches[i] - spec.min) * (spec.length - TRACK) / (spec.max - spec.min);
        lv_obj_set_pos(n, v ? 2 : at + 3, v ? spec.length - TRACK - at + 3 : 2);
        lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
    }
    if (spec.ticks > 0) {
        lv_obj_t *sc = lv_scale_create(m_root);
        lv_scale_set_mode(sc, v ? LV_SCALE_MODE_VERTICAL_RIGHT : LV_SCALE_MODE_HORIZONTAL_BOTTOM);
        lv_scale_set_range(sc, spec.min, spec.max);
        lv_scale_set_total_tick_count(sc, static_cast<std::uint32_t>(spec.ticks));
        lv_scale_set_major_tick_every(sc, static_cast<std::uint32_t>(spec.major));
        lv_scale_set_label_show(sc, false);
        lv_obj_set_size(sc, v ? 14 : spec.length - TRACK, v ? spec.length - TRACK : 14);
        lv_obj_set_pos(sc, v ? KNOB + 2 : TRACK / 2, v ? TRACK / 2 : KNOB + 2);
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
    if (m_fill != nullptr) {
        lv_obj_set_style_bg_color(m_fill, colour(Ink::Accent), 0);
    }
    lv_obj_set_style_bg_color(m_knob, m_spec.fill ? lv_color_hex(0xffffff) : colour(Ink::Accent), 0);
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
    const std::int32_t span = m_spec.length - TRACK;
    const std::int32_t at = (std::clamp(value, m_spec.min, m_spec.max) - m_spec.min) * span / (m_spec.max - m_spec.min);
    if (m_fill != nullptr) {
        lv_obj_set_size(m_fill, v ? TRACK : at + TRACK, v ? at + TRACK : TRACK);
        lv_obj_set_pos(m_fill, 0, v ? m_spec.length - at - TRACK : 0);
    }
    const std::int32_t knob_at = at + TRACK / 2 - KNOB / 2;
    lv_obj_set_pos(m_knob, v ? 0 : knob_at, v ? m_spec.length - knob_at - KNOB : 0);
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
    const std::int32_t grow = code == LV_EVENT_RELEASED ? 256 : 300;
    lv_obj_set_style_transform_scale_x(rail->m_knob, grow, 0);
    lv_obj_set_style_transform_scale_y(rail->m_knob, grow, 0);
    if (code == LV_EVENT_RELEASED) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t box;
    lv_obj_get_coords(rail->m_track, &box);
    const bool v = rail->m_spec.vertical;
    const std::int32_t span = rail->m_spec.length - TRACK;
    const std::int32_t pos = v ? box.y2 - TRACK / 2 - p.y : p.x - box.x1 - TRACK / 2;
    const std::int32_t value = rail->m_spec.min + std::clamp<std::int32_t>(pos, 0, span) * (rail->m_spec.max - rail->m_spec.min) / span;
    rail->set_value(value, code == LV_EVENT_PRESSED);
    lv_obj_send_event(rail->m_root, LV_EVENT_VALUE_CHANGED, nullptr);
}

}  // namespace ui::kit
