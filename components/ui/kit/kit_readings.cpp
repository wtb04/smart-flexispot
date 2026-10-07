#include "kit.h"

#include <algorithm>

namespace ui::kit {
namespace {

constexpr std::int32_t BAR = 6, DOT = 20, RING = 4, GAP = 4;
constexpr lv_opa_t     FAINT = 70;

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

}  // namespace

Scale::Scale(lv_obj_t *parent, std::int32_t width, std::int32_t min, std::int32_t max, const Zone *zones, int count)
    : m_zones(zones), m_count(std::min(count, 6)), m_min(min), m_max(std::max(max, min + 1)), m_w(width)
{
    m_root = plain(parent);
    lv_obj_set_size(m_root, width > 0 ? width : LV_PCT(100), DOT);
    lv_obj_add_flag(m_root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);  // the dot's ring at either end
    for (int i = 0; i < m_count; ++i) {
        lv_obj_t *band = m_bands[i] = plain(m_root);
        lv_obj_set_style_radius(band, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(band, FAINT, 0);
    }
    lv_obj_add_event_cb(m_root, [](lv_event_t *e) { static_cast<Scale *>(lv_event_get_user_data(e))->lay_out(); },
                        LV_EVENT_SIZE_CHANGED, this);
    m_dot = plain(m_root);
    lv_obj_set_size(m_dot, DOT, DOT);
    lv_obj_set_style_radius(m_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(m_dot, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_width(m_dot, RING, 0);
    lv_obj_set_hidden(m_dot, true);
    restyle();
    on_restyle(m_root, [](void *ctx) { static_cast<Scale *>(ctx)->restyle(); }, this);
    lv_obj_add_event_cb(m_root, [](lv_event_t *e) { delete static_cast<Scale *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, this);
}

Scale::~Scale() { lv_anim_delete(this, nullptr); }

void Scale::lay_out()
{
    m_w = lv_obj_get_width(m_root);
    const auto x_of = [&](std::int32_t v) { return (std::clamp(v, m_min, m_max) - m_min) * m_w / (m_max - m_min); };
    for (int i = 0; i < m_count; ++i) {
        const std::int32_t x0 = x_of(m_zones[i].from) + (i == 0 ? 0 : GAP / 2);
        const std::int32_t x1 = x_of(m_zones[i].to) - (i == m_count - 1 ? 0 : GAP / 2);
        lv_obj_set_size(m_bands[i], std::max<std::int32_t>(x1 - x0, 1), BAR);
        lv_obj_set_pos(m_bands[i], x0, (DOT - BAR) / 2);
    }
    if (m_shown != INT32_MIN) {
        place(m_shown);
    }
}

int Scale::zone_of(std::int32_t value) const
{
    for (int i = 0; i < m_count; ++i) {
        if (value >= m_zones[i].from && value <= m_zones[i].to) {
            return i;
        }
    }
    return -1;
}

void Scale::restyle()
{
    for (int i = 0; i < m_count; ++i) {
        lv_obj_set_style_bg_color(m_bands[i], colour(m_zones[i].ink), 0);
    }
    const int lit = zone_of(m_value);
    lv_obj_set_style_border_color(m_dot, lit >= 0 ? colour(m_zones[lit].ink) : colour(Ink::Secondary), 0);
}

void Scale::place(std::int32_t value)
{
    m_shown = value;
    const std::int32_t x = (std::clamp(value, m_min, m_max) - m_min) * m_w / (m_max - m_min) - DOT / 2;
    if (lv_obj_get_style_x(m_dot, LV_PART_MAIN) != x) {
        lv_obj_set_x(m_dot, x);
    }
}

void Scale::on_move(void *var, int32_t v) { static_cast<Scale *>(var)->place(v); }

void Scale::set_value(std::int32_t value, bool animate)
{
    const std::int32_t from = m_shown;
    m_value = value;
    const int lit = zone_of(value);
    if (lit != m_lit) {
        m_lit = lit;
        for (int i = 0; i < m_count; ++i) {
            lv_obj_set_style_bg_opa(m_bands[i], i == lit ? LV_OPA_COVER : FAINT, 0);
        }
        restyle();
    }
    lv_anim_delete(this, nullptr);
    if (lv_obj_is_hidden(m_dot)) {
        lv_obj_set_hidden(m_dot, false);
        animate = false;
    }
    if (!animate || from == INT32_MIN || from == value) {
        place(value);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_values(&a, from, value);
    lv_anim_set_exec_cb(&a, on_move);
    lv_anim_set_duration(&a, motion::move_ms);
    lv_anim_set_path_cb(&a, motion::spring);
    lv_anim_start(&a);
}

Reading::Reading(lv_obj_t *parent, const ReadingSpec &spec) : m_spec(spec)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);
    if (spec.caption != nullptr) {
        text(parent, Role::Small, spec.caption, Ink::Secondary);
    }
    // the unit beside the figure rather than where the widest figure would end
    lv_obj_t *line = plain(parent);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(line, space::xs, 0);
    m_figure = text(line, Role::ReadXL, "--");
    if (spec.unit != nullptr) {
        m_unit = text(line, Role::Body, spec.unit, Ink::Secondary);
        lv_obj_set_style_pad_bottom(m_unit, space::m, 0);  // on the figures' baseline, near enough
    }
    m_word = text(parent, Role::H3, "", Ink::Secondary);
    lv_obj_t *gap = plain(parent);
    lv_obj_set_flex_grow(gap, 1);
    m_scale = new Scale(parent, 0, spec.min, spec.max, spec.zones, spec.zone_count);
    if (spec.note != nullptr) {
        lv_obj_t *n = text(parent, Role::Small, spec.note, Ink::Tertiary);
        lv_obj_set_style_margin_top(n, space::s, 0);
    }
    lv_obj_add_event_cb(parent, [](lv_event_t *e) {
        auto *r = static_cast<Reading *>(lv_event_get_user_data(e));
        lv_anim_delete(r, nullptr);
        delete r;
    }, LV_EVENT_DELETE, this);
}

void Reading::show(std::int32_t value)
{
    if (value == m_shown) {
        return;
    }
    m_shown = value;
    lv_label_set_text_fmt(m_figure, "%d", static_cast<int>(value));
}

void Reading::set_value(std::int32_t value, bool animate)
{
    const std::int32_t from = m_shown;
    lv_anim_delete(this, nullptr);
    if (!animate || from == INT32_MIN) {
        show(value);
    } else {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_values(&a, from, value);
        lv_anim_set_exec_cb(&a, [](void *r, int32_t v) { static_cast<Reading *>(r)->show(v); });
        lv_anim_set_duration(&a, motion::move_ms);
        lv_anim_set_path_cb(&a, motion::settle);
        lv_anim_start(&a);
    }
    m_scale->set_value(value, animate);
    int zone = -1;
    for (int i = 0; i < m_spec.zone_count; ++i) {
        if (value >= m_spec.zones[i].from && value <= m_spec.zones[i].to) {
            zone = i;
        }
    }
    if (zone == m_zone) {
        return;
    }
    m_zone = zone;
    const char *word = zone >= 0 && m_spec.zones[zone].word != nullptr ? m_spec.zones[zone].word : "";
    lv_label_set_text(m_word, word);
    set_ink(m_word, zone >= 0 ? m_spec.zones[zone].ink : Ink::Secondary);
}

void Reading::clear()
{
    lv_anim_delete(this, nullptr);
    m_shown = INT32_MIN;
    lv_label_set_text(m_figure, "--");
    lv_label_set_text(m_word, "");
    m_zone = -2;
}

}  // namespace ui::kit
