#include "kit.h"

#include "kit_fonts.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ui::kit {
namespace {

constexpr std::int32_t ROW_H = 64, GROUP_PAD = space::xl, LIST_PAD = space::m;
constexpr std::int32_t SQUARE = 36, SQUARE_RADIUS = 10, MARK_W = 4, BADGE_H = 32, BADGE_PAD = space::m;
constexpr std::uint32_t LATE_MS  = 80;  // how long a finger rests on a row under a scroll before it lights
constexpr std::uint32_t FLASH_MS = 90;  // a tap quicker than that still shows

bool       s_ready = false;
lv_style_t s_row, s_row_group, s_row_list, s_fill, s_fill_lit, s_fill_open;
lv_style_t s_text_col, s_scroller, s_scrollbar, s_section, s_square, s_badge, s_badge_text;
lv_style_t s_tint[8];
lv_style_transition_dsc_t s_lit_tr, s_unlit_tr;
constexpr lv_style_prop_t FILL_PROPS[] = {LV_STYLE_BG_OPA, LV_STYLE_PROP_INV};

const lv_font_t *font_of(Role role)
{
    switch (role) {
    case Role::Label: return fonts::label();
    case Role::Small: return fonts::small();
    case Role::Body: return fonts::body();
    case Role::H3: return fonts::h3();
    case Role::H2: return fonts::h2();
    case Role::H1: return fonts::h1();
    case Role::ReadS: return fonts::read_s();
    case Role::ReadM: return fonts::read_m();
    case Role::ReadL: return fonts::read_l();
    case Role::ReadXL: return fonts::read_xl();
    case Role::ReadXXL: return fonts::read_xxl();
    }
    return fonts::body();
}

std::uint32_t next_letter(const char *&s)
{
    const auto *u = reinterpret_cast<const unsigned char *>(s);
    if (u[0] < 0x80) {
        s += 1;
        return u[0];
    }
    const int n = u[0] >= 0xf0 ? 4 : u[0] >= 0xe0 ? 3 : 2;
    std::uint32_t c = u[0] & (0x3f >> (n - 1));
    for (int i = 1; i < n && (u[i] & 0xc0) == 0x80; ++i) {
        c = (c << 6) | (u[i] & 0x3f);
    }
    s += n;
    return c;
}

std::int32_t width_of(const lv_font_t *font, const char *text, std::int32_t letter_space)
{
    std::int32_t w = 0;
    const char  *s = text;
    std::uint32_t c = *s != '\0' ? next_letter(s) : 0;
    while (c != 0) {
        const std::uint32_t next = *s != '\0' ? next_letter(s) : 0;
        w += lv_font_get_glyph_width(font, c, next) + (next != 0 ? letter_space : 0);
        c = next;
    }
    return w;
}

lv_style_t &tint(Ink ink) { return s_tint[static_cast<int>(ink)]; }

void init_styles()
{
    for (lv_style_t *st : {&s_row, &s_row_group, &s_row_list, &s_fill, &s_fill_lit, &s_fill_open, &s_text_col,
                           &s_scroller, &s_scrollbar, &s_section, &s_square, &s_badge, &s_badge_text}) {
        lv_style_init(st);
    }
    for (lv_style_t &st : s_tint) {
        lv_style_init(&st);
    }
    lv_style_transition_dsc_init(&s_lit_tr, FILL_PROPS, lv_anim_path_linear, motion::press_ms, 0, nullptr);
    lv_style_transition_dsc_init(&s_unlit_tr, FILL_PROPS, motion::spring, motion::release_ms, 0, nullptr);

    lv_style_set_min_height(&s_row, ROW_H);
    lv_style_set_pad_ver(&s_row, space::s);
    lv_style_set_pad_column(&s_row, 14);
    lv_style_set_pad_hor(&s_row_group, GROUP_PAD);
    lv_style_set_pad_hor(&s_row_list, LIST_PAD);

    lv_style_set_bg_opa(&s_fill, LV_OPA_TRANSP);
    lv_style_set_transition(&s_fill, &s_unlit_tr);
    lv_style_set_bg_opa(&s_fill_lit, LV_OPA_COVER);
    lv_style_set_transition(&s_fill_lit, &s_lit_tr);
    lv_style_set_bg_opa(&s_fill_open, LV_OPA_COVER);

    lv_style_set_pad_row(&s_text_col, 1);

    lv_style_set_pad_right(&s_scroller, 0);
    lv_style_set_pad_bottom(&s_scroller, space::s);
    lv_style_set_width(&s_scrollbar, 4);
    lv_style_set_radius(&s_scrollbar, LV_RADIUS_CIRCLE);
    lv_style_set_pad_right(&s_scrollbar, 2);
    lv_style_set_pad_ver(&s_scrollbar, space::m);

    lv_style_set_pad_top(&s_section, space::m);
    lv_style_set_pad_bottom(&s_section, space::s);

    lv_style_set_size(&s_square, SQUARE, SQUARE);
    lv_style_set_radius(&s_square, SQUARE_RADIUS);

    lv_style_set_height(&s_badge, BADGE_H);
    lv_style_set_radius(&s_badge, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&s_badge, BADGE_PAD);
    lv_style_set_pad_column(&s_badge, 4);
    lv_style_set_text_font(&s_badge_text, fonts::small());
    s_ready = true;
}

}  // namespace

void restyle_lists()
{
    if (!s_ready) {
        init_styles();
    }
    const Palette &p    = colours();
    const bool     dark = look() == Look::Dark;
    // the kit's press: the surface sinks to the colour of a well
    const lv_color_t sunk = lv_color_hex(dark ? p.well : p.raised_hi);
    for (lv_style_t *st : {&s_fill, &s_fill_lit, &s_fill_open}) {
        lv_style_set_bg_color(st, sunk);
    }
    lv_style_set_bg_color(&s_scrollbar, lv_color_hex(p.ink));
    lv_style_set_bg_opa(&s_scrollbar, p.ink3_opa);

    const lv_opa_t tinted = dark ? 46 : 38;
    for (int i = 0; i < 8; ++i) {
        const Ink   ink = static_cast<Ink>(i);
        lv_style_t &st  = s_tint[i];
        if (ink == Ink::OnAccent) {
            lv_style_set_bg_color(&st, lv_color_hex(p.accent));
            lv_style_set_bg_opa(&st, LV_OPA_COVER);
            lv_style_set_text_color(&st, lv_color_hex(p.on_accent));
            lv_style_set_text_opa(&st, LV_OPA_COVER);
        } else if (ink == Ink::Primary || ink == Ink::Secondary || ink == Ink::Tertiary) {
            lv_style_set_bg_color(&st, lv_color_hex(p.hair));
            lv_style_set_bg_opa(&st, static_cast<lv_opa_t>(p.hair_opa * 2));
            lv_style_set_text_color(&st, lv_color_hex(p.ink));
            lv_style_set_text_opa(&st, ink == Ink::Primary ? LV_OPA_COVER : ink == Ink::Secondary ? p.ink2_opa : p.ink3_opa);
        } else {
            lv_style_set_bg_color(&st, colour(ink));
            lv_style_set_bg_opa(&st, tinted);
            lv_style_set_text_color(&st, colour(ink));
            lv_style_set_text_opa(&st, LV_OPA_COVER);
        }
    }
}

std::int32_t widest(Role role, std::initializer_list<const char *> texts)
{
    const lv_font_t   *font = font_of(role);
    const std::int32_t ls   = role == Role::Label ? 1 : 0;
    std::int32_t       w    = 0;
    for (const char *t : texts) {
        if (t != nullptr) {
            w = std::max(w, width_of(font, t, ls));
        }
    }
    return w + 1;
}

namespace {

// --- rows -----------------------------------------------------------------

struct Row {
    lv_obj_t   *title = nullptr, *detail = nullptr, *value = nullptr, *trail = nullptr;
    lv_timer_t *timer = nullptr;
    std::int32_t inset = 0;   // where its hairline starts
    bool grouped  = false;    // edge to edge in a group's tile, so its ends are the tile's corners
    bool pressable = false;
    bool toggles  = false;
    bool shown    = false;    // lit at some point of this press
};

void on_row_draw(lv_event_t *e);

Row *row_of(lv_obj_t *obj)
{
    if (obj == nullptr || lv_obj_get_event_count(obj) == 0 ||
        lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, 0)) != on_row_draw) {
        return nullptr;
    }
    return static_cast<Row *>(lv_event_dsc_get_user_data(lv_obj_get_event_dsc(obj, 0)));
}

lv_obj_t *neighbour(lv_obj_t *obj, int dir)
{
    lv_obj_t *o = lv_obj_get_sibling(obj, dir);
    while (o != nullptr && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) {
        o = lv_obj_get_sibling(o, dir);
    }
    return o;
}

// The press as a fill drawn here, not as the row's own background: a row in a
// group's tile has two of its corners round and two square, which no radius
// gives, and the tile clipping its rows to its corners would put every frame
// of them through a masked layer.
void on_row_draw(lv_event_t *e)
{
    lv_obj_t      *obj   = lv_event_get_current_target_obj(e);
    const Row     *row   = static_cast<Row *>(lv_event_get_user_data(e));
    lv_layer_t    *layer = lv_event_get_layer(e);
    const Palette &p     = colours();
    lv_area_t      c;
    lv_obj_get_coords(obj, &c);
    const lv_opa_t opa  = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    lv_obj_t      *prev = neighbour(obj, -1);

    if (row_of(prev) != nullptr) {
        lv_draw_rect_dsc_t hair;
        lv_draw_rect_dsc_init(&hair);
        hair.bg_color = lv_color_hex(p.hair);
        hair.bg_opa   = static_cast<lv_opa_t>(p.hair_opa * opa / 255);
        const lv_area_t line{c.x1 + row->inset, c.y1, c.x2, c.y1};
        lv_draw_rect(layer, &hair, &line);
    }

    const lv_opa_t lit = lv_obj_get_style_bg_opa(obj, LV_PART_SELECTED);
    if (lit <= LV_OPA_MIN) {
        return;
    }
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = lv_obj_get_style_bg_color(obj, LV_PART_SELECTED);
    fill.bg_opa   = static_cast<lv_opa_t>(lit * opa / 255);
    lv_area_t at = c;
    if (row->grouped) {
        const bool first = prev == nullptr, last = neighbour(obj, 1) == nullptr;
        fill.radius = first || last ? radius::tile : 0;
        if (first && !last) {
            at.y2 += radius::tile;  // round at the top, cut square at the row's foot
        } else if (last && !first) {
            at.y1 -= radius::tile;
        }
    } else {
        fill.radius = radius::inner;
    }
    const lv_area_t clip = layer->_clip_area;
    if (lv_area_intersect(&layer->_clip_area, &clip, &c)) {
        lv_draw_rect(layer, &fill, &at);
    }
    layer->_clip_area = clip;
}

void light(lv_obj_t *obj, bool on)
{
    if (lv_obj_has_state(obj, LV_STATE_USER_2) == on) {
        return;
    }
    if (on) {
        if (Row *row = row_of(obj)) {
            row->shown = true;
        }
        lv_obj_add_state(obj, LV_STATE_USER_2);
        lv_anim_refr_now();  // the press in the frame the touch is read for, as a button's
    } else {
        lv_obj_remove_state(obj, LV_STATE_USER_2);
    }
}

bool under_scroll(lv_obj_t *obj)
{
    for (lv_obj_t *o = lv_obj_get_parent(obj); o != nullptr; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) &&
            (lv_obj_get_scroll_top(o) > 0 || lv_obj_get_scroll_bottom(o) > 0)) {
            return true;
        }
    }
    return false;
}

void drop_timer(Row *row)
{
    if (row->timer != nullptr) {
        lv_timer_delete(row->timer);
        row->timer = nullptr;
    }
}

void after(lv_obj_t *obj, Row *row, std::uint32_t ms, lv_timer_cb_t cb)
{
    drop_timer(row);
    row->timer = lv_timer_create(cb, ms, obj);
    lv_timer_set_repeat_count(row->timer, 1);
    lv_timer_set_auto_delete(row->timer, false);
}

void on_late(lv_timer_t *t)
{
    auto *obj = static_cast<lv_obj_t *>(lv_timer_get_user_data(t));
    Row  *row = row_of(obj);
    drop_timer(row);
    if (lv_obj_has_state(obj, LV_STATE_PRESSED)) {
        light(obj, true);
    }
}

void on_flash(lv_timer_t *t)
{
    auto *obj = static_cast<lv_obj_t *>(lv_timer_get_user_data(t));
    drop_timer(row_of(obj));
    light(obj, false);
}

void on_row_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    Row      *row = row_of(obj);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        row->shown = false;
        if (under_scroll(obj)) {
            after(obj, row, LATE_MS, on_late);
        } else {
            light(obj, true);
        }
        break;
    case LV_EVENT_STATE_CHANGED:
        // a scroll taking the finger over takes the pressed state without a release
        if (!lv_obj_has_state(obj, LV_STATE_PRESSED) && row->timer == nullptr) {
            light(obj, false);
        }
        break;
    case LV_EVENT_CLICKED:
        if (!row->shown) {
            light(obj, true);
            after(obj, row, FLASH_MS, on_flash);
        }
        if (row->toggles) {
            lv_obj_t  *sw = row->trail;
            const bool on = !lv_obj_has_state(sw, LV_STATE_CHECKED);
            lv_obj_set_state(sw, LV_STATE_CHECKED, on);
            lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
        }
        break;
    case LV_EVENT_DELETE:
        drop_timer(row);
        delete row;
        break;
    default:
        break;
    }
}

lv_obj_t *bare_box(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *one_line(lv_obj_t *parent, Role role, const char *value, Ink ink, std::int32_t width)
{
    lv_obj_t *label = text(parent, role, value, ink);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    // dots only shorten a label of fixed height; fixed, it never lays anything out again either
    lv_obj_set_size(label, width, font_of(role)->line_height);
    return label;
}

void set_text_if(lv_obj_t *label, const char *value)
{
    if (label != nullptr && std::strcmp(lv_label_get_text(label), value != nullptr ? value : "") != 0) {
        lv_label_set_text(label, value != nullptr ? value : "");
    }
}

}  // namespace

lv_obj_t *section(lv_obj_t *parent, const char *title, const char *aside)
{
    lv_obj_t *sec = bare_box(parent);
    lv_obj_add_style(sec, &s_section, 0);
    const std::int32_t pad = lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE) ? LIST_PAD : GROUP_PAD;
    lv_obj_set_style_pad_hor(sec, pad, 0);
    lv_obj_set_size(sec, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sec, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    text(sec, Role::Label, title, Ink::Secondary);
    if (aside != nullptr) {
        text(sec, Role::Label, aside, Ink::Tertiary);
    }
    return sec;
}

lv_obj_t *group(lv_obj_t *parent, std::int32_t width)
{
    lv_obj_t *g = tile(parent, Lift::Low);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_size(g, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_COLUMN);
    return g;
}

// The column scrolls inside the tile's padding, where the tile's rounded
// corners are clear of it: the tile then covers all of it, so a frame of
// scrolling starts at the tile's flat fill and nothing under the tile is drawn.
lv_obj_t *list(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *t = tile(parent, Lift::Low);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_pad_all(t, space::s, 0);
    lv_obj_t *col = lv_obj_create(t);
    lv_obj_remove_style_all(col);
    lv_obj_add_style(col, &s_scroller, 0);
    lv_obj_add_style(col, &s_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_ACTIVE);
    return col;
}

lv_obj_t *row(lv_obj_t *parent, const RowSpec &spec)
{
    auto *data     = new Row{};
    data->grouped  = !lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    data->toggles  = spec.trail == Trail::Toggle;
    data->pressable = spec.trail == Trail::Chevron || data->toggles || spec.action || spec.pressable;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_event_cb(obj, on_row_draw, LV_EVENT_DRAW_MAIN, data);  // first: row_of knows a row by it
    lv_obj_add_style(obj, &s_row, 0);
    lv_obj_add_style(obj, data->grouped ? &s_row_group : &s_row_list, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (data->pressable) {
        lv_obj_add_style(obj, &s_fill, LV_PART_SELECTED);
        lv_obj_add_style(obj, &s_fill_lit, LV_PART_SELECTED | LV_STATE_USER_2);
        lv_obj_add_style(obj, &s_fill_open, LV_PART_SELECTED | LV_STATE_USER_1);  // its popover is open
        for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_STATE_CHANGED, LV_EVENT_CLICKED}) {
            lv_obj_add_event_cb(obj, on_row_event, code, nullptr);
        }
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(obj, on_row_event, LV_EVENT_DELETE, nullptr);

    const std::int32_t gap = lv_obj_get_style_pad_column(obj, LV_PART_MAIN);
    std::int32_t       x   = lv_obj_get_style_pad_left(obj, LV_PART_MAIN);
    if (spec.glyph != nullptr && spec.bare) {
        icon(obj, spec.glyph, 28, false, spec.tint);
        x += 28 + gap;
    } else if (spec.glyph != nullptr) {
        lv_obj_t *sq = bare_box(obj);
        lv_obj_add_style(sq, &s_square, 0);
        lv_obj_add_style(sq, &tint(spec.tint), 0);
        lv_obj_t *g = lv_label_create(sq);
        lv_obj_set_style_text_font(g, fonts::icon(24, true), 0);
        lv_label_set_text(g, spec.glyph);
        lv_obj_center(g);
        x += SQUARE + gap;
    }
    if (spec.lead != nullptr) {
        const std::int32_t w = spec.lead_w > 0 ? spec.lead_w : widest(Role::Body, {spec.lead});
        one_line(obj, Role::Body, spec.lead, Ink::Primary, w);
        x += w + gap;
    }
    if (spec.mark != 0) {
        lv_obj_t *bar = bare_box(obj);
        lv_obj_set_size(bar, MARK_W, spec.detail != nullptr ? 40 : 26);
        lv_obj_set_style_radius(bar, MARK_W / 2, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(spec.mark), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        x += MARK_W + gap;
    }
    data->inset = x;

    lv_obj_t *col = bare_box(obj);
    lv_obj_add_style(col, &s_text_col, 0);
    lv_obj_set_size(col, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    data->title = one_line(col, Role::Body, spec.title, spec.action ? Ink::Accent : Ink::Primary, LV_PCT(100));
    if (spec.detail != nullptr) {
        data->detail = one_line(col, Role::Small, spec.detail, Ink::Secondary, LV_PCT(100));
    }

    if (spec.value != nullptr && spec.trail != Trail::Badge) {
        const std::int32_t w = spec.value_w > 0 ? spec.value_w : widest(Role::Body, {spec.value});
        data->value = one_line(obj, Role::Body, spec.value, Ink::Secondary, w);
        lv_obj_set_style_text_align(data->value, LV_TEXT_ALIGN_RIGHT, 0);
        data->trail = data->value;
    }
    switch (spec.trail) {
    case Trail::Chevron:
        icon(obj, glyph::chevron_right, 24, false, Ink::Tertiary);
        lv_obj_set_style_margin_right(lv_obj_get_child(obj, -1), -6, 0);  // its box is wider than its stroke
        break;
    case Trail::Toggle:
        data->trail = toggle(obj, spec.on);
        break;
    case Trail::Badge:
        data->trail = badge(obj, spec.value != nullptr ? spec.value : "", spec.badge, nullptr, spec.value_w);
        break;
    default:
        break;
    }
    return obj;
}

void set_row_value(lv_obj_t *obj, const char *value)
{
    if (Row *row = row_of(obj)) {
        set_text_if(row->value, value);
    }
}

void set_row_detail(lv_obj_t *obj, const char *detail)
{
    if (Row *row = row_of(obj)) {
        set_text_if(row->detail, detail);
    }
}

lv_obj_t *row_trail(lv_obj_t *obj)
{
    const Row *row = row_of(obj);
    return row != nullptr ? row->trail : nullptr;
}

// --- badges ---------------------------------------------------------------

lv_obj_t *badge(lv_obj_t *parent, const char *value, Ink ink, const char *glyph, std::int32_t width)
{
    lv_obj_t *b = bare_box(parent);
    lv_obj_add_style(b, &s_badge, 0);
    lv_obj_add_style(b, &tint(ink), 0);
    lv_obj_set_user_data(b, reinterpret_cast<void *>(static_cast<std::intptr_t>(ink)));
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    std::int32_t inner = 0;
    if (glyph != nullptr) {
        lv_obj_t *g = lv_label_create(b);
        lv_obj_set_style_text_font(g, fonts::icon(24, true), 0);
        lv_obj_set_style_margin_left(g, -4, 0);
        lv_label_set_text(g, glyph);
        inner += 24 - 4 + lv_obj_get_style_pad_column(b, LV_PART_MAIN);
    }
    lv_obj_t *t = lv_label_create(b);
    lv_obj_add_style(t, &s_badge_text, 0);
    lv_label_set_text(t, value);
    const std::int32_t text_w = width > 0 ? width - 2 * BADGE_PAD - inner : widest(Role::Small, {value});
    lv_obj_set_size(t, std::max<std::int32_t>(text_w, 1), fonts::small()->line_height);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(b, width > 0 ? width : text_w + 2 * BADGE_PAD + inner);
    return b;
}

void set_badge(lv_obj_t *b, const char *value, Ink ink)
{
    const auto was = static_cast<Ink>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(b)));
    if (was != ink) {
        lv_obj_remove_style(b, &tint(was), 0);
        lv_obj_add_style(b, &tint(ink), 0);
        lv_obj_set_user_data(b, reinterpret_cast<void *>(static_cast<std::intptr_t>(ink)));
    }
    set_text_if(lv_obj_get_child(b, -1), value);
}

// --- empty ----------------------------------------------------------------

lv_obj_t *empty(lv_obj_t *parent, const char *glyph, const char *value)
{
    lv_obj_t *box = bare_box(parent);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(box, space::xxl, 0);
    lv_obj_set_style_pad_hor(box, space::l, 0);
    lv_obj_set_style_pad_row(box, space::s, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icon(box, glyph, 34, false, Ink::Tertiary);
    lv_obj_t *t = text(box, Role::Body, value, Ink::Tertiary);
    lv_obj_set_width(t, LV_PCT(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    return box;
}

// --- readouts -------------------------------------------------------------

namespace {
void format(char *out, std::size_t size, std::int32_t value, std::int32_t decimals, const char *unit)
{
    const char *sign = value < 0 ? "-" : "";
    const auto  mag  = static_cast<long>(value < 0 ? -value : value);
    int         n    = decimals > 0 ? std::snprintf(out, size, "%s%ld.%ld", sign, mag / 10, mag % 10)
                                    : std::snprintf(out, size, "%s%ld", sign, mag);
    if (unit != nullptr && n > 0 && static_cast<std::size_t>(n) < size) {
        std::snprintf(out + n, size - static_cast<std::size_t>(n), " %s", unit);
    }
}
}  // namespace

Readout::Readout(lv_obj_t *parent, const ReadoutSpec &spec) : m_spec(spec)
{
    char lo[16], hi[16];
    format(lo, sizeof lo, spec.min, spec.decimals, nullptr);
    format(hi, sizeof hi, spec.max, spec.decimals, nullptr);
    const lv_font_t   *font   = font_of(spec.role);
    const std::int32_t read_w = widest(spec.role, {lo, hi, "--"});
    const std::int32_t gap    = font->line_height / 8;
    const Role         unit_role = spec.role >= Role::ReadL ? Role::H3 : Role::Small;
    const lv_font_t   *unit_font = font_of(unit_role);
    const std::int32_t unit_w = spec.unit != nullptr ? widest(unit_role, {spec.unit}) : 0;

    // no layout in it at all: every part placed once, at a size that holds
    m_root = bare_box(parent);
    m_read = text(m_root, spec.role, "--");
    lv_obj_set_size(m_read, read_w, font->line_height);
    lv_obj_set_style_text_align(m_read, LV_TEXT_ALIGN_RIGHT, 0);
    std::int32_t w = read_w, h = font->line_height;
    if (spec.unit != nullptr) {
        lv_obj_t *u = text(m_root, unit_role, spec.unit, Ink::Secondary);
        lv_obj_set_size(u, unit_w, unit_font->line_height);
        // on the figures' baseline
        const std::int32_t y = (font->line_height - font->base_line) - (unit_font->line_height - unit_font->base_line);
        lv_obj_set_pos(u, read_w + gap, y);
        w += gap + unit_w;
    }
    if (spec.caption != nullptr) {
        lv_obj_t *c = text(m_root, Role::Label, spec.caption, Ink::Secondary);
        const std::int32_t cw = std::max(w, widest(Role::Label, {spec.caption}));
        lv_obj_set_size(c, cw, fonts::label()->line_height);
        lv_obj_set_pos(c, 0, h);
        w = cw;
        h += fonts::label()->line_height;
    }
    lv_obj_set_size(m_root, w, h);
}

Readout::~Readout() { lv_anim_delete(this, nullptr); }

void Readout::on_count(void *var, int32_t v) { static_cast<Readout *>(var)->show(v); }

void Readout::show(std::int32_t value)
{
    if (value == m_shown) {
        return;
    }
    m_shown = value;
    char buf[16];
    format(buf, sizeof buf, value, m_spec.decimals, nullptr);
    set_text_if(m_read, buf);
}

void Readout::set_value(std::int32_t value, bool animate)
{
    const bool was_clear = m_shown == INT32_MIN;
    const std::int32_t from = was_clear ? value : m_shown;
    m_value = value;
    lv_anim_delete(this, on_count);
    if (!animate || was_clear || from == value) {
        show(value);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_values(&a, from, value);
    lv_anim_set_exec_cb(&a, on_count);
    lv_anim_set_duration(&a, motion::move_ms);
    lv_anim_set_path_cb(&a, motion::spring);
    lv_anim_start(&a);
}

void Readout::clear()
{
    lv_anim_delete(this, on_count);
    m_shown = INT32_MIN;
    set_text_if(m_read, "--");
}

// --- steppers -------------------------------------------------------------

Stepper::Stepper(lv_obj_t *parent, const StepperSpec &spec) : m_spec(spec), m_value(spec.value)
{
    m_root = bare_box(parent);
    lv_obj_set_size(m_root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(m_root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(m_root, space::xs, 0);
    m_less = button(m_root, Button::Raised, glyph::remove, nullptr, true);
    lv_obj_set_size(m_less, size::button, size::button);
    char lo[24], hi[24];
    format(lo, sizeof lo, spec.min, spec.decimals, spec.unit);
    format(hi, sizeof hi, spec.max, spec.decimals, spec.unit);
    m_read = text(m_root, Role::ReadS, "");
    lv_obj_set_size(m_read, widest(Role::ReadS, {lo, hi}) + space::s, fonts::read_s()->line_height);
    lv_obj_set_style_text_align(m_read, LV_TEXT_ALIGN_CENTER, 0);
    m_more = button(m_root, Button::Raised, glyph::add, nullptr, true);
    lv_obj_set_size(m_more, size::button, size::button);
    for (lv_obj_t *b : {m_less, m_more}) {
        for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
            lv_obj_add_event_cb(b, on_press, code, this);
        }
    }
    lv_obj_add_event_cb(
        m_root, [](lv_event_t *e) { static_cast<Stepper *>(lv_event_get_user_data(e))->stop(); }, LV_EVENT_DELETE,
        this);
    show();
}

Stepper::~Stepper() { stop(); }

void Stepper::show()
{
    char buf[24];
    format(buf, sizeof buf, m_value, m_spec.decimals, m_spec.unit);
    set_text_if(m_read, buf);
    set_usable(m_less, m_value > m_spec.min);
    set_usable(m_more, m_value < m_spec.max);
}

void Stepper::set_value(std::int32_t value)
{
    value = std::clamp(value, m_spec.min, m_spec.max);
    if (value == m_value) {
        return;
    }
    m_value = value;
    show();
}

void Stepper::stop()
{
    if (m_timer != nullptr) {
        lv_timer_delete(m_timer);
        m_timer = nullptr;
    }
    m_dir = 0;
}

void Stepper::step(int dir, int times)
{
    const std::int32_t by = m_spec.step * times;
    // a held step lands on its multiples, so ten at a time goes 70, 80, 90 and not 73, 83
    std::int32_t to = m_value + dir * by;
    if (times > 1) {
        to = (to - m_spec.min) / by * by + m_spec.min;
    }
    to = std::clamp(to, m_spec.min, m_spec.max);
    if (to == m_value) {
        stop();
        return;
    }
    m_value = to;
    show();
    lv_obj_send_event(m_root, LV_EVENT_VALUE_CHANGED, nullptr);
    if (m_value == m_spec.min || m_value == m_spec.max) {
        stop();
    }
}

void Stepper::on_press(lv_event_t *e)
{
    auto *self = static_cast<Stepper *>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        self->stop();
        return;
    }
    self->stop();
    self->m_dir     = lv_event_get_current_target_obj(e) == self->m_more ? 1 : -1;
    self->m_repeats = 0;
    self->step(self->m_dir, 1);
    if (self->m_dir != 0) {
        self->m_timer = lv_timer_create(on_repeat, 450, self);
    }
}

// Held: a step every 160 ms, then every 90, then 55; over a long range,
// two and then ten at a time.
void Stepper::on_repeat(lv_timer_t *t)
{
    auto *self = static_cast<Stepper *>(lv_timer_get_user_data(t));
    const int  n     = ++self->m_repeats;
    const auto steps = (self->m_spec.max - self->m_spec.min) / std::max<std::int32_t>(self->m_spec.step, 1);
    lv_timer_set_period(t, n < 5 ? 160 : n < 14 ? 90 : 55);
    const int times = n >= 30 && steps >= 200 ? 10 : n >= 14 && steps >= 40 ? 2 : 1;
    self->step(self->m_dir, times);
}

// --- sheets ---------------------------------------------------------------

// The body scrolls inside a well, which covers it: on glass, every frame of
// scrolling would be the backdrop, the glass over it and then the text.
lv_obj_t *sheet(lv_obj_t *source, const char *title, std::int32_t w, std::int32_t h)
{
    lv_obj_t *body = popover(source, w, h, From::Centre);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);
    lv_obj_t *head = bare_box(body);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(head, Role::H2, title);
    lv_obj_t *close = button(head, Button::Flat, glyph::close, nullptr, true);
    lv_obj_set_size(close, size::button, size::button);
    lv_obj_set_style_margin_right(close, -space::s, 0);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_popover(true); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *frame = well(body);
    lv_obj_set_width(frame, LV_PCT(100));
    lv_obj_set_flex_grow(frame, 1);
    lv_obj_set_style_pad_all(frame, space::s, 0);
    lv_obj_t *col = lv_obj_create(frame);
    lv_obj_remove_style_all(col);
    lv_obj_add_style(col, &s_scroller, 0);
    lv_obj_add_style(col, &s_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_ACTIVE);
    return col;
}

lv_obj_t *sheet_row(lv_obj_t *body, const char *label, const char *value)
{
    RowSpec spec;
    spec.title = label;
    spec.trail = Trail::Value;
    spec.value = value;
    lv_obj_t *r = row(body, spec);
    lv_obj_set_style_min_height(r, 52, 0);
    lv_obj_t *v = row_trail(r);
    lv_obj_set_width(v, LV_PCT(55));  // its share of the line rather than its text's width: nothing moves as it changes
    return v;
}

}  // namespace ui::kit
