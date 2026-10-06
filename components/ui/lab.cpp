#include "lab.h"

#include "kit.h"
#include "kit_fonts.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ui::lab {
namespace {

using namespace ui::kit;
namespace ic = ui::kit::glyph;

constexpr std::int32_t CONTENT_X = 142, CONTENT_Y = 104, CONTENT_W = 1122, CONTENT_H = 600;
constexpr std::int32_t SHEET_Y = 76, SHEET_H = CONTENT_H - SHEET_Y;
constexpr std::uint32_t ACCENTS[] = {0, 0x5aa9ff, 0x7ccf6a, 0xb48cff, 0xff6f8e, 0x3fd0c0};
constexpr int GLASS_STEPS[] = {0, 35, 50, 70, 100};

lv_obj_t *s_sheets[5]{};
lv_obj_t *s_perf = nullptr;
lv_obj_t *s_light_item = nullptr, *s_bench_btn = nullptr, *s_bench_seg = nullptr, *s_slab = nullptr;
lv_obj_t *s_picker = nullptr;
lv_obj_t *s_state  = nullptr;
int       s_accent = 0;
int       s_glass_step = 2;
bool      s_blur_scrim = true;

Dial *s_co2 = nullptr, *s_hum = nullptr, *s_heat = nullptr, *s_focus = nullptr;
Rail *s_glass_rail = nullptr;
int   s_focus_lit = 1;

lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow, std::int32_t gap)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(obj, flow);
    lv_obj_set_style_pad_row(obj, gap, 0);
    lv_obj_set_style_pad_column(obj, gap, 0);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return obj;
}

lv_obj_t *spacer(lv_obj_t *parent)
{
    lv_obj_t *obj = box(parent, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_flex_grow(obj, 1);
    return obj;
}

void show_state()
{
    if (s_state == nullptr) {
        return;
    }
    char buf[112];
    std::snprintf(buf, sizeof buf, "%s, glass %d%%, popovers %s", look() == Look::Dark ? "Dark" : "Light", glass(),
                  pop_glass() == PopGlass::Frozen ? (pop_motion() == PopMotion::Rise ? "frozen, rising" : "frozen, growing")
                                                  : s_blur_scrim ? "live" : "live, scrim plain");
    lv_label_set_text(s_state, buf);
}

void choose_sheet(int index)
{
    lv_obj_scroll_to_y(layer(Layer::Content), 0, LV_ANIM_OFF);
    for (int i = 0; i < 5; ++i) {
        if (s_sheets[i] != nullptr) {
            if (i == index) {
                lv_obj_remove_flag(s_sheets[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_sheets[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_picker != nullptr && segmented_chosen(s_picker) != index) {
        segmented_choose(s_picker, index, true);
    }
}

template <typename T> void own(lv_obj_t *owner, T *thing)
{
    lv_obj_add_event_cb(owner, [](lv_event_t *e) { delete static_cast<T *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, thing);
}

// --- popovers -------------------------------------------------------------

lv_obj_t *head(lv_obj_t *body, const char *title, const char *sub)
{
    lv_obj_t *row = box(body, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *col = box(row, LV_FLEX_FLOW_COLUMN, 2);
    text(col, Role::H2, title);
    if (sub != nullptr) {
        text(col, Role::Small, sub, Ink::Secondary);
    }
    spacer(row);
    return row;
}

void open_lights_from(lv_obj_t *source)
{
    lv_obj_t *body = popover(source, 430, 380, From::Below, s_blur_scrim);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);
    lv_obj_t *row = head(body, "Lights", "2 of 4 on");
    lv_obj_t *off = button(row, Button::Raised, ic::power_settings_new, "All off");
    lv_obj_set_height(off, 48);
    lv_obj_t *grid = box(body, LV_FLEX_FLOW_ROW_WRAP, space::m);
    lv_obj_set_width(grid, LV_PCT(100));
    struct L { const char *glyph, *name; bool on; };
    for (const L &l : {L{ic::desk, "Desk lamp", true}, L{ic::wall_lamp, "Wall lamp", true},
                       L{ic::floor_lamp, "Floor lamp", false}, L{ic::light, "Main lamp", false}}) {
        lv_obj_t *t = lamp_tile(grid, l.glyph, l.name, l.on);
        lv_obj_set_size(t, 185, 120);
    }
}

void open_lights(lv_event_t *e) { open_lights_from(lv_event_get_current_target_obj(e)); }

void open_heating(lv_event_t *e)
{
    lv_obj_t *body = popover(lv_event_get_current_target_obj(e), 440, 540, From::Below, s_blur_scrim);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, space::l, 0);
    head(body, "Heating", "Now 20.5 °C");
    DialSpec spec;
    spec.size = 290, spec.min = 150, spec.max = 250, spec.start = 135, spec.sweep = 270;
    spec.ticks = 21, spec.major = 2, spec.decimals = 1, spec.needle = true, spec.fill = true, spec.target = true;
    spec.unit = "°C NOW", spec.read = Role::ReadL;
    auto *dial = new Dial(body, spec);
    own(dial->obj(), dial);
    dial->set_value(205, false);
    dial->set_target(205, false);
    dial->set_target(210, true);
    // the readout shows the target here: what a tap changes
    lv_obj_t *row = box(body, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(row, LV_PCT(100));
    for (int dir : {-1, 1}) {
        lv_obj_t *b = button(row, Button::Raised, dir < 0 ? ic::remove : ic::add, nullptr);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 64);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *ev) {
                auto *d   = static_cast<Dial *>(lv_event_get_user_data(ev));
                const int step = lv_obj_get_index(lv_event_get_current_target_obj(ev)) == 0 ? -5 : 5;
                static int target = 210;
                target = std::clamp(target + step, 150, 250);
                d->set_target(target, true);
            },
            LV_EVENT_CLICKED, dial);
    }
    lv_obj_t *mode = segmented(body, {"Heat", "Off"}, 0, 392);
    (void)mode;
}

void open_desk(lv_event_t *e)
{
    lv_obj_t *body = popover(lv_event_get_current_target_obj(e), 420, 600, From::Beside, s_blur_scrim);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);
    head(body, "Desk", "At Sit, 74.0 cm");
    lv_obj_t *row = box(body, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    static const std::int32_t presets[] = {660, 700, 740, 1120};
    RailSpec rs;
    rs.length = 420, rs.vertical = true, rs.min = 660, rs.max = 1310, rs.ticks = 14, rs.major = 2;
    rs.notches = presets, rs.notch_count = 4, rs.fill = true;
    auto *rail = new Rail(row, rs);
    own(rail->obj(), rail);
    rail->set_value(740, false);
    lv_obj_t *grid = box(row, LV_FLEX_FLOW_ROW_WRAP, space::m);
    lv_obj_set_width(grid, 306);
    for (int dir : {1, -1}) {
        lv_obj_t *hold = button(grid, Button::Raised, dir > 0 ? ic::keyboard_arrow_up : ic::keyboard_arrow_down, "Hold");
        lv_obj_set_size(hold, 147, 64);
    }
    struct P { const char *name; std::int32_t mm; };
    for (const P &p : {P{"Stand", 1120}, P{"Sit", 740}, P{"Ultra low", 660}, P{"Preset 1", 700}}) {
        lv_obj_t *t = button(grid, Button::Raised, nullptr, nullptr, false, true);
        lv_obj_set_size(t, 147, 150);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(t, space::l, 0);
        lv_obj_t *n = lv_label_create(t);
        lv_obj_set_style_text_font(n, fonts::h3(), 0);
        lv_label_set_text(n, p.name);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d.%d", static_cast<int>(p.mm / 10), static_cast<int>(p.mm % 10));
        lv_obj_t *v = lv_label_create(t);
        lv_obj_set_style_text_font(v, fonts::read_s(), 0);
        lv_label_set_text(v, buf);
        if (p.mm == 740) {
            lv_obj_add_state(t, LV_STATE_CHECKED);
        }
        lv_obj_set_user_data(t, reinterpret_cast<void *>(static_cast<std::intptr_t>(p.mm)));
        lv_obj_add_event_cb(
            t,
            [](lv_event_t *ev) {
                lv_obj_t *tile = lv_event_get_current_target_obj(ev);
                lv_obj_t *g    = lv_obj_get_parent(tile);
                for (std::uint32_t i = 0; i < lv_obj_get_child_count(g); ++i) {
                    lv_obj_t *other = lv_obj_get_child(g, static_cast<int32_t>(i));
                    if (other != tile) {
                        lv_obj_remove_state(other, LV_STATE_CHECKED);
                    }
                }
                lv_obj_add_state(tile, LV_STATE_CHECKED);
                static_cast<Rail *>(lv_event_get_user_data(ev))
                    ->set_value(static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(tile))), true);
            },
            LV_EVENT_CLICKED, rail);
    }
    text(body, Role::Small, "Hold a preset to save the height it goes to", Ink::Secondary);
}

void open_media(lv_event_t *e)
{
    lv_obj_t *body = popover(lv_event_get_current_target_obj(e), 480, 340, From::Below, s_blur_scrim);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);
    lv_obj_t *top = box(body, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_t *art = tile(top, Lift::Low);
    lv_obj_set_size(art, 120, 120);
    lv_obj_set_style_radius(art, radius::inner, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(0x1b1f6b), 0);
    lv_obj_set_style_bg_grad_color(art, lv_color_hex(0x3a0f52), 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_center(icon(art, ic::music_note, 34, true, Ink::Primary));
    lv_obj_t *col = box(top, LV_FLEX_FLOW_COLUMN, 4);
    text(col, Role::Label, "SPOTIFY, OFFICE SPEAKER", Ink::Secondary);
    text(col, Role::H2, "Midnight City");
    text(col, Role::Body, "M83", Ink::Secondary);
    RailSpec rs;
    rs.length = 432, rs.min = 0, rs.max = 243, rs.draggable = true;
    auto *scrub = new Rail(body, rs);
    own(scrub->obj(), scrub);
    scrub->set_value(102, false);
    lv_obj_t *row = box(body, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, space::l, 0);
    lv_obj_t *prev = button(row, Button::Raised, ic::skip_previous, nullptr, true);
    lv_obj_set_size(prev, 72, 72);
    lv_obj_t *pause = button(row, Button::Accent, ic::pause, nullptr, true);
    lv_obj_set_size(pause, 200, 72);
    lv_obj_t *next = button(row, Button::Raised, ic::skip_next, nullptr, true);
    lv_obj_set_size(next, 72, 72);
}

// --- chrome ---------------------------------------------------------------

void build_dock(lv_obj_t *scr)
{
    lv_obj_t *dock = glass(scr, Glass::Large);
    lv_obj_set_style_radius(dock, radius::dock, 0);
    lv_obj_set_pos(dock, 14, 14);
    lv_obj_set_size(dock, size::dock, size::screen_h - 28);
    lv_obj_set_style_pad_all(dock, space::m, 0);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(dock, space::m, 0);

    lv_obj_t *height = button(dock, Button::Flat, nullptr, nullptr);
    lv_obj_set_size(height, 88, 104);
    lv_obj_set_style_pad_all(height, 0, 0);
    lv_obj_set_style_pad_row(height, 2, 0);
    lv_obj_set_flex_flow(height, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(height, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *w = well(height);
    lv_obj_add_flag(w, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(w, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(w, 88, 104);
    lv_obj_set_style_radius(w, 24, 0);
    lv_obj_t *read = text(height, Role::ReadS, "74.0");
    lv_obj_add_flag(read, LV_OBJ_FLAG_EVENT_BUBBLE);
    text(height, Role::Label, "CM", Ink::Tertiary);
    lv_obj_add_event_cb(height, open_desk, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *stand = button(dock, Button::Raised, nullptr, nullptr);
    lv_obj_set_size(stand, 88, 80);
    lv_obj_set_style_radius(stand, 24, 0);
    lv_obj_t *col = box(stand, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icon(col, ic::arrow_upward, 24, false);
    icon(col, ic::desk, 34, false);

    spacer(dock);
    lv_obj_t *tabs[4];
    const char *glyphs[4] = {ic::home, ic::calendar_month, ic::flight, ic::timer};
    for (int i = 0; i < 4; ++i) {
        if (i == 1) {
            spacer(dock);
        }
        if (i == 3) {
            hairline(dock, false, 40);
        }
        tabs[i] = button(dock, Button::Flat, glyphs[i], nullptr, false, true);
        lv_obj_set_size(tabs[i], 88, 80);
        lv_obj_set_style_radius(tabs[i], 24, 0);
        lv_obj_set_style_text_opa(tabs[i], colours().ink2_opa, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(tabs[i], 0), fonts::icon(34, false), 0);
        lv_obj_set_style_text_font(lv_obj_get_child(tabs[i], 0), fonts::icon(34, true), LV_STATE_CHECKED);
        lv_obj_add_event_cb(
            tabs[i],
            [](lv_event_t *e) {
                lv_obj_t *tab  = lv_event_get_current_target_obj(e);
                lv_obj_t *dock_of = lv_obj_get_parent(tab);
                for (std::uint32_t k = 0; k < lv_obj_get_child_count(dock_of); ++k) {
                    lv_obj_t *other = lv_obj_get_child(dock_of, static_cast<int32_t>(k));
                    if (other != tab && lv_obj_has_flag(other, LV_OBJ_FLAG_CHECKABLE)) {
                        lv_obj_remove_state(other, LV_STATE_CHECKED);
                    }
                }
                lv_obj_add_state(tab, LV_STATE_CHECKED);
            },
            LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_add_state(tabs[0], LV_STATE_CHECKED);
}

void build_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = box(scr, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_pos(bar, CONTENT_X, 16);
    lv_obj_set_size(bar, CONTENT_W, size::bar);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *status = box(bar, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(status, space::m, 0);
    text(status, Role::H1, "9:41");
    icon(status, ic::wifi, 28, false, Ink::Secondary);
    icon(status, ic::smartphone, 28, false, Ink::Secondary);
    spacer(bar);

    lv_obj_t *room  = capsule(bar);
    lv_obj_t *light = s_light_item = capsule_item(room, ic::lightbulb, "2", 118);
    lv_obj_set_style_text_font(lv_obj_get_child(light, 0), fonts::icon(28, true), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(light, 0), lv_color_hex(colours().lamp), 0);
    lv_obj_add_event_cb(light, open_lights, LV_EVENT_CLICKED, nullptr);
    capsule_divider(room);
    lv_obj_t *heat = capsule_item(room, ic::local_fire_department, "20.5°", 150);
    lv_obj_set_style_text_color(lv_obj_get_child(heat, 0), colour(Ink::Accent), 0);
    lv_obj_add_event_cb(heat, open_heating, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *media = capsule(bar);
    lv_obj_set_width(media, 452);
    lv_obj_set_style_pad_column(media, space::m, 0);
    lv_obj_add_flag(media, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *art = tile(media, Lift::Low);
    lv_obj_set_size(art, 52, 52);
    lv_obj_set_style_radius(art, 12, 0);
    lv_obj_set_style_pad_all(art, 0, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(0x1b1f6b), 0);
    lv_obj_set_style_bg_grad_color(art, lv_color_hex(0x3a0f52), 0);
    lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *col = box(media, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_grow(col, 1);
    text(col, Role::H3, "Midnight City");
    text(col, Role::Small, "M83", Ink::Secondary);
    lv_obj_t *pp = button(media, Button::Raised, ic::pause, nullptr, true);
    lv_obj_set_size(pp, 56, 56);
    lv_obj_add_event_cb(media, open_media, LV_EVENT_CLICKED, nullptr);
}

// --- sheets ---------------------------------------------------------------

lv_obj_t *sheet(lv_obj_t *scr)
{
    lv_obj_t *s = box(scr, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_pos(s, CONTENT_X, CONTENT_Y + SHEET_Y);
    lv_obj_set_size(s, CONTENT_W, SHEET_H);
    return s;
}

lv_obj_t *column_tile(lv_obj_t *parent, const char *title, std::int32_t w)
{
    lv_obj_t *t = tile(parent, Lift::Low);
    lv_obj_set_size(t, w, SHEET_H);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, space::m, 0);
    if (title != nullptr) {
        text(t, Role::Label, title, Ink::Secondary);
    }
    return t;
}

void build_surfaces(lv_obj_t *scr)
{
    lv_obj_t *s = s_sheets[0] = sheet(scr);
    lv_obj_t *type = column_tile(s, "TYPE", 380);
    lv_obj_set_style_pad_row(type, 6, 0);
    text(type, Role::ReadL, "21.5°");
    text(type, Role::H1, "Good evening");
    text(type, Role::H2, "Desk presets");
    text(type, Role::H3, "Computing and Communications");
    text(type, Role::Body, "The wash is done");
    text(type, Role::Small, "Boeing 747, 65 km south", Ink::Secondary);
    text(type, Role::Label, "LECTURE, TOMORROW", Ink::Secondary);
    lv_obj_t *inks = box(type, LV_FLEX_FLOW_ROW_WRAP, space::m);
    lv_obj_set_width(inks, LV_PCT(100));
    lv_obj_set_style_pad_top(inks, space::m, 0);
    struct I { const char *name; Ink ink; };
    for (const I &i : {I{"Primary", Ink::Primary}, I{"Secondary", Ink::Secondary}, I{"Tertiary", Ink::Tertiary},
                       I{"Accent", Ink::Accent}, I{"Good", Ink::Green}, I{"Fair", Ink::Amber}, I{"Poor", Ink::Red}}) {
        text(inks, Role::Small, i.name, i.ink);
    }

    lv_obj_t *mid = box(s, LV_FLEX_FLOW_COLUMN, space::l);
    lv_obj_set_size(mid, 340, SHEET_H);
    lv_obj_t *low = tile(mid, Lift::Low);
    lv_obj_set_size(low, LV_PCT(100), 150);
    text(low, Role::Label, "TILE", Ink::Secondary);
    lv_obj_align(text(low, Role::Body, "Flat, on one soft shadow"), LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *high = tile(mid, Lift::High);
    lv_obj_set_size(high, LV_PCT(100), 150);
    text(high, Role::Label, "RAISED TILE", Ink::Secondary);
    lv_obj_align(text(high, Role::Body, "For what sits above the rest"), LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *holder = tile(mid, Lift::Low);
    lv_obj_set_size(holder, LV_PCT(100), 192);
    lv_obj_set_flex_flow(holder, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(holder, space::m, 0);
    text(holder, Role::Label, "WELL", Ink::Secondary);
    lv_obj_t *w = well(holder);
    lv_obj_set_size(w, LV_PCT(100), 100);
    lv_obj_align(text(w, Role::ReadS, "74.0"), LV_ALIGN_LEFT_MID, space::s, 0);
    lv_obj_align(text(w, Role::Small, "Sunk in", Ink::Secondary), LV_ALIGN_RIGHT_MID, -space::s, 0);

    lv_obj_t *right = box(s, LV_FLEX_FLOW_COLUMN, space::l);
    lv_obj_set_size(right, 370, SHEET_H);
    lv_obj_t *under = tile(right, Lift::Low);
    lv_obj_set_size(under, LV_PCT(100), 250);
    text(under, Role::Label, "GLASS OVER A TILE", Ink::Secondary);
    lv_obj_align(text(under, Role::ReadXL, "640"), LV_ALIGN_LEFT_MID, 0, 6);
    lv_obj_t *lamp = lamp_tile(under, ic::desk, "Desk lamp", true);
    lv_obj_set_size(lamp, 150, 110);
    lv_obj_align(lamp, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_t *pane = glass(right, Glass::Small);
    lv_obj_add_flag(pane, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_radius(pane, radius::round, 0);
    lv_obj_set_size(pane, 250, 72);
    lv_obj_set_pos(pane, 60, 120);
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pane, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pane, space::s, 0);
    icon(pane, ic::lightbulb, 28, true);
    text(pane, Role::H3, "Frosted glass");

    lv_obj_t *icons = tile(right, Lift::Low);
    lv_obj_set_size(icons, LV_PCT(100), 250);
    lv_obj_set_flex_flow(icons, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(icons, space::l, 0);
    lv_obj_set_style_pad_column(icons, 13, 0);
    lv_obj_t *cap = text(icons, Role::Label, "ICONS, OFF AND ON", Ink::Secondary);
    lv_obj_set_width(cap, LV_PCT(100));
    const char *glyphs[] = {ic::lightbulb, ic::desk, ic::timer, ic::flight, ic::calendar_month, ic::home,
                            ic::wifi, ic::notifications};
    for (bool filled : {false, true}) {
        for (const char *g : glyphs) {
            icon(icons, g, 28, filled, filled ? Ink::Primary : Ink::Secondary);
        }
    }
}

void build_controls(lv_obj_t *scr)
{
    lv_obj_t *s = s_sheets[1] = sheet(scr);

    lv_obj_t *buttons = column_tile(s, "BUTTONS", 360);
    lv_obj_t *wrap    = box(buttons, LV_FLEX_FLOW_ROW_WRAP, space::m);
    lv_obj_set_width(wrap, LV_PCT(100));
    s_bench_btn = button(wrap, Button::Raised, nullptr, "Raised");
    button(wrap, Button::Raised, ic::keyboard_arrow_up, "Hold");
    button(wrap, Button::Accent, ic::pause, "Pause");
    button(wrap, Button::Soft, nullptr, "Soft");
    lv_obj_t *r1 = button(wrap, Button::Raised, ic::add, nullptr, true);
    lv_obj_set_width(r1, 56);
    lv_obj_t *r2 = button(wrap, Button::Accent, ic::play_arrow, nullptr, true);
    lv_obj_set_width(r2, 56);
    lv_obj_t *r3 = button(wrap, Button::Raised, ic::restart_alt, nullptr, true);
    lv_obj_set_width(r3, 56);
    button(wrap, Button::Raised, ic::bedtime, "Tap to turn on", false, true);
    lv_obj_t *no = button(wrap, Button::Raised, nullptr, "Unusable");
    set_usable(no, false);
    lv_obj_t *hint = text(buttons, Role::Small, "Press and hold any of them: they sink in and spring back.", Ink::Tertiary);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *choices = column_tile(s, "CHOICES", 360);
    lv_obj_t *looks   = segmented(choices, {"Dark", "Light"}, look() == Look::Dark ? 0 : 1, 320);
    lv_obj_add_event_cb(
        looks, [](lv_event_t *e) { set_look(segmented_chosen(lv_event_get_target_obj(e)) == 0 ? Look::Dark : Look::Light); show_state(); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    s_bench_seg = segmented(choices, {"Normal", "Flipped", "Auto"}, 0, 320);
    for (const char *name : {"While I'm away, use Room", "Dimmer on battery"}) {
        lv_obj_t *row = box(choices, LV_FLEX_FLOW_ROW, space::m);
        lv_obj_set_size(row, LV_PCT(100), 56);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        text(row, Role::Body, name);
        toggle(row, name[0] == 'W');
    }
    lv_obj_t *lamps = box(choices, LV_FLEX_FLOW_ROW, space::m);
    for (bool on : {true, false}) {
        lv_obj_t *t = lamp_tile(lamps, on ? ic::desk : ic::floor_lamp, on ? "Desk lamp" : "Floor lamp", on);
        lv_obj_set_size(t, 152, 110);
    }

    lv_obj_t *rails = column_tile(s, "RAILS", 370);
    text(rails, Role::Small, "Brightness, dragged: no ticks on a slider", Ink::Secondary);
    RailSpec bright;
    bright.length = 320, bright.draggable = true;
    auto *b = new Rail(rails, bright);
    own(b->obj(), b);
    b->set_value(80, false);
    text(rails, Role::Small, "The desk's travel: ticks where a height is read", Ink::Secondary);
    static const std::int32_t presets[] = {660, 700, 740, 1120};
    RailSpec travel;
    travel.length = 320, travel.min = 660, travel.max = 1310, travel.ticks = 14, travel.major = 2;
    travel.notches = presets, travel.notch_count = 4;
    auto *t = new Rail(rails, travel);
    own(t->obj(), t);
    t->set_value(740, false);
    lv_obj_t *go = box(rails, LV_FLEX_FLOW_ROW, space::m);
    struct G { const char *name; std::int32_t mm; };
    for (const G &g : {G{"Sit", 740}, G{"Stand", 1120}, G{"Ultra low", 660}}) {
        lv_obj_t *btn = button(go, Button::Raised, nullptr, g.name);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(static_cast<std::intptr_t>(g.mm)));
        lv_obj_add_event_cb(
            btn,
            [](lv_event_t *e) {
                static_cast<Rail *>(lv_event_get_user_data(e))
                    ->set_value(static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(
                                    lv_obj_get_user_data(lv_event_get_current_target_obj(e)))),
                                true);
            },
            LV_EVENT_CLICKED, t);
    }
    text(rails, Role::Small, "Progress: a plain track", Ink::Secondary);
    RailSpec prog;
    prog.length = 320, prog.min = 0, prog.max = 1504;
    auto *p = new Rail(rails, prog);
    own(p->obj(), p);
    p->set_value(21, false);
}

const Zone CO2_ZONES[] = {{400, 800, Ink::Green}, {800, 1200, Ink::Amber}, {1200, 2000, Ink::Red}};
const Zone HUM_ZONES[] = {{0, 30, Ink::Red}, {30, 40, Ink::Amber}, {40, 60, Ink::Green}, {60, 70, Ink::Amber}, {70, 100, Ink::Red}};

lv_obj_t *dial_tile(lv_obj_t *parent, const char *title, const char *note)
{
    lv_obj_t *t = column_tile(parent, title, 268);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_t *n = text(t, Role::Small, note, Ink::Tertiary);
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    return t;
}

void build_instruments(lv_obj_t *scr)
{
    lv_obj_t *s = s_sheets[2] = sheet(scr);

    lv_obj_t *t1 = dial_tile(s, "CO2", "Zones on the rim; only the one the needle is in is bright. It wanders, to see it move.");
    DialSpec co2;
    co2.size = 224, co2.min = 400, co2.max = 2000, co2.ticks = 31, co2.major = 5, co2.zones = CO2_ZONES;
    co2.zone_count = 3, co2.ends = true, co2.unit = "PPM", co2.read = Role::ReadM;
    s_co2 = new Dial(t1, co2);
    own(s_co2->obj(), s_co2);
    s_co2->set_value(640, false);

    lv_obj_t *t2 = dial_tile(s, "HUMIDITY", "Good is in the middle, so the green sits there.");
    DialSpec hum = co2;
    hum.min = 0, hum.max = 100, hum.zones = HUM_ZONES, hum.zone_count = 5, hum.unit = "%";
    s_hum = new Dial(t2, hum);
    own(s_hum->obj(), s_hum);
    s_hum->set_value(48, false);

    lv_obj_t *t3 = dial_tile(s, "HEATING", "A setting: the accent arc and the knob are the target, the needle is the room.");
    DialSpec heat;
    heat.size = 224, heat.min = 150, heat.max = 250, heat.start = 135, heat.sweep = 270, heat.ticks = 21;
    heat.major = 2, heat.decimals = 1, heat.fill = true, heat.target = true, heat.unit = "°C NOW";
    heat.read = Role::ReadM;
    s_heat = new Dial(t3, heat);
    own(s_heat->obj(), s_heat);
    s_heat->set_value(205, false);
    s_heat->set_target(210, false);
    lv_obj_t *row = box(t3, LV_FLEX_FLOW_ROW, space::m);
    for (int dir : {-1, 1}) {
        lv_obj_t *b = button(row, Button::Raised, dir < 0 ? ic::remove : ic::add, nullptr, true);
        lv_obj_set_size(b, 96, 56);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
                const int step = lv_obj_get_index(lv_event_get_current_target_obj(e)) == 0 ? -5 : 5;
                static int target = 210;
                target = std::clamp(target + step, 150, 250);
                s_heat->set_target(target, true);
            },
            LV_EVENT_CLICKED, nullptr);
    }

    lv_obj_t *t4 = dial_tile(s, "FOCUS", "A tick a minute, lit as the round goes. No needle: the time is the reading.");
    DialSpec focus;
    focus.size = 224, focus.min = 0, focus.max = 24, focus.start = 270, focus.sweep = 346, focus.ticks = 25;
    focus.major = 5, focus.needle = false, focus.read = Role::ReadM, focus.unit = "UNTIL 10:06";
    s_focus = new Dial(t4, focus);
    own(s_focus->obj(), s_focus);
    lv_label_set_text(lv_obj_get_child(lv_obj_get_child(s_focus->obj(), -1), 0), "24:51");
    s_focus->set_lit(1);

    lv_timer_create(
        [](lv_timer_t *) {
            if (s_co2 != nullptr) {
                s_co2->set_value(500 + std::rand() % 1100, true);
            }
            if (s_hum != nullptr) {
                s_hum->set_value(25 + std::rand() % 55, true);
            }
            if (s_heat != nullptr) {
                s_heat->set_value(195 + std::rand() % 25, true);
            }
            if (s_focus != nullptr) {
                s_focus_lit = s_focus_lit % 25 + 1;
                s_focus->set_lit(s_focus_lit);
            }
        },
        2600, nullptr);
}

void build_glass(lv_obj_t *scr)
{
    lv_obj_t *s = s_sheets[3] = sheet(scr);
    lv_obj_t *under = box(s, LV_FLEX_FLOW_ROW_WRAP, space::l);
    lv_obj_set_size(under, 752, SHEET_H);

    const char *reads[] = {"640", "120", "48", "20.5°", "13:45", "74.0"};
    for (int i = 0; i < 6; ++i) {
        lv_obj_t *t = tile(under, Lift::Low);
        lv_obj_set_size(t, 240, 160);
        lv_obj_align(text(t, Role::ReadL, reads[i]), LV_ALIGN_BOTTOM_LEFT, 0, 0);
        text(t, Role::Label, "BEHIND THE GLASS", Ink::Secondary);
    }
    lv_obj_t *lamps = box(under, LV_FLEX_FLOW_ROW, space::l);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *t = lamp_tile(lamps, ic::desk, i == 0 ? "Desk lamp" : "Wall lamp", true);
        lv_obj_set_size(t, 240, 160);
    }

    lv_obj_t *side = tile(s, Lift::Low);
    lv_obj_set_size(side, 330, SHEET_H);
    lv_obj_add_flag(side, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(side, CONTENT_W - 330, 0);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, space::l, 0);
    text(side, Role::Label, "GLASS", Ink::Secondary);
    text(side, Role::Small, "From clear to nearly solid. Drag it and watch the slab and the bar.", Ink::Secondary);
    lv_obj_set_width(lv_obj_get_child(side, 1), LV_PCT(100));
    lv_label_set_long_mode(lv_obj_get_child(side, 1), LV_LABEL_LONG_MODE_WRAP);
    RailSpec rs;
    rs.length = 286, rs.draggable = true;
    s_glass_rail = new Rail(side, rs);
    own(s_glass_rail->obj(), s_glass_rail);
    s_glass_rail->set_value(glass(), false);
    lv_obj_add_event_cb(
        s_glass_rail->obj(), [](lv_event_t *) { set_glass(s_glass_rail->value()); show_state(); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *n = button(side, Button::Raised, ic::notifications, "A notice");
    lv_obj_add_event_cb(
        n, [](lv_event_t *) { notice("HOME ASSISTANT", ic::local_laundry_service, "Washing machine", "The wash is done", 6000); },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *p = button(side, Button::Raised, ic::lightbulb, "The lights");
    lv_obj_add_event_cb(p, open_lights, LV_EVENT_CLICKED, nullptr);
    text(side, Role::Label, "POPOVERS OPEN", Ink::Secondary);
    lv_obj_t *motion = segmented(side, {"Growing", "Rising"}, pop_motion() == PopMotion::Grow ? 0 : 1, 286);
    lv_obj_add_event_cb(
        motion, [](lv_event_t *e) { set_pop_motion(segmented_chosen(lv_event_get_target_obj(e)) == 0 ? PopMotion::Grow : PopMotion::Rise); show_state(); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    text(side, Role::Label, "OVER", Ink::Secondary);
    lv_obj_t *how = segmented(side, {"A still picture", "Live blur"}, pop_glass() == PopGlass::Frozen ? 0 : 1, 286);
    lv_obj_add_event_cb(
        how, [](lv_event_t *e) { set_pop_glass(segmented_chosen(lv_event_get_target_obj(e)) == 0 ? PopGlass::Frozen : PopGlass::Live); show_state(); },
        LV_EVENT_VALUE_CHANGED, nullptr);

    // a slab to drag over the tiles, to see the blur follow
    lv_obj_t *slab = s_slab = glass(s, Glass::Large);
    lv_obj_add_flag(slab, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_radius(slab, radius::pop, 0);
    lv_obj_set_size(slab, 320, 190);
    lv_obj_set_pos(slab, 120, 90);
    lv_obj_add_flag(slab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(slab, space::xxl, 0);
    text(slab, Role::H2, "Drag me");
    lv_obj_align(text(slab, Role::Small, "A popover-sized slab of glass", Ink::Secondary), LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(
        slab,
        [](lv_event_t *e) {
            lv_obj_t *obj = lv_event_get_target_obj(e);
            lv_point_t v;
            lv_indev_get_vect(lv_indev_active(), &v);
            lv_obj_set_pos(obj, lv_obj_get_x(obj) + v.x, lv_obj_get_y(obj) + v.y);
        },
        LV_EVENT_PRESSING, nullptr);
}

void build_layers(lv_obj_t *parent)
{
    lv_obj_t *s = s_sheets[4] = sheet(parent);
    lv_obj_set_height(s, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_t *intro = tile(s, Lift::Low);
    lv_obj_set_size(intro, CONTENT_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(intro, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(intro, space::s, 0);
    text(intro, Role::Label, "LAYERS", Ink::Secondary);
    lv_obj_t *n = text(intro, Role::Body,
                       "Scroll this sheet. The content slides under the bar and past the dock, which stay above it: "
                       "frosted, casting onto what passes beneath, with an edge fading in under the bar.");
    lv_obj_set_width(n, LV_PCT(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    const char *reads[] = {"640", "120", "48", "20.5°", "13:45", "74.0", "37,000", "404", "17", "24:51", "112.0", "66.0"};
    for (int i = 0; i < 12; ++i) {
        if (i == 4 || i == 9) {
            lv_obj_t *t = lamp_tile(s, ic::desk, i == 4 ? "Desk lamp" : "Wall lamp", true);
            lv_obj_set_size(t, 363, 200);
            continue;
        }
        lv_obj_t *t = tile(s, Lift::Low);
        lv_obj_set_size(t, 363, 200);
        text(t, Role::Label, "UNDER THE CHROME", Ink::Secondary);
        lv_obj_align(text(t, Role::ReadL, reads[i]), LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
}

// --- bench ----------------------------------------------------------------

struct Phase {
    const char *name;
    std::uint32_t ms;
    void (*setup)();
    void (*tick)(std::uint32_t at, std::uint32_t frame);
};

void no_tick(std::uint32_t, std::uint32_t) {}

const Phase PHASES[] = {
    {"idle", 800, [] { choose_sheet(1); }, no_tick},
    {"button press", 1400, nullptr,
     [](std::uint32_t at, std::uint32_t) {
         if (at % 200 < 16) {
             if (lv_obj_has_state(s_bench_btn, LV_STATE_PRESSED)) {
                 lv_obj_remove_state(s_bench_btn, LV_STATE_PRESSED);
             } else {
                 lv_obj_add_state(s_bench_btn, LV_STATE_PRESSED);
             }
         }
     }},
    {"segmented", 1600, nullptr,
     [](std::uint32_t at, std::uint32_t) {
         if (at % 400 < 16) {
             segmented_choose(s_bench_seg, static_cast<int>(at / 400 % 3), true);
         }
     }},
    {"dials", 2000, [] { choose_sheet(2); },
     [](std::uint32_t at, std::uint32_t) {
         if (at % 500 < 16) {
             s_co2->set_value(500 + std::rand() % 1100, true);
             s_hum->set_value(25 + std::rand() % 55, true);
             s_heat->set_target(160 + std::rand() % 80, true);
         }
     }},
    {"popover, live", 1300, [] { choose_sheet(0); set_pop_glass(PopGlass::Live); },
     [](std::uint32_t at, std::uint32_t frame) {
         if (frame == 0) {
             open_lights_from(s_light_item);
         } else if (at >= 800 && popover_open()) {
             close_popover(true);
         }
     }},
    {"popover, frozen", 1300, [] { set_pop_glass(PopGlass::Frozen); set_pop_motion(PopMotion::Grow); },
     [](std::uint32_t at, std::uint32_t frame) {
         if (frame == 0) {
             open_lights_from(s_light_item);
         } else if (at >= 800 && popover_open()) {
             close_popover(true);
         }
     }},
    {"popover, rise", 1300, [] { set_pop_glass(PopGlass::Frozen); set_pop_motion(PopMotion::Rise); },
     [](std::uint32_t at, std::uint32_t frame) {
         if (frame == 0) {
             open_lights_from(s_light_item);
         } else if (at >= 800 && popover_open()) {
             close_popover(true);
         }
     }},
    {"drag glass", 1000, [] { set_pop_motion(PopMotion::Grow); choose_sheet(3); },
     [](std::uint32_t at, std::uint32_t) {
         lv_obj_set_x(s_slab, 60 + static_cast<std::int32_t>(at < 500 ? at : 1000 - at) * 3 / 5);
     }},
    {"scroll under bar", 1600, [] { choose_sheet(4); },
     [](std::uint32_t at, std::uint32_t) {
         lv_obj_scroll_to_y(layer(Layer::Content), static_cast<std::int32_t>(at < 800 ? at : 1600 - at) * 3 / 4, LV_ANIM_OFF);
     }},
    {"notice", 1200, [] { choose_sheet(0); },
     [](std::uint32_t, std::uint32_t frame) {
         if (frame == 0) {
             notice("HOME ASSISTANT", glyph::local_laundry_service, "Washing machine", "The wash is done", 900);
         }
     }},
};

char s_report[4000] = "no bench run yet\n";
char s_frames[3][200]{};

struct Config {
    const char *name;
    bool        blur, shadows;
};
constexpr Config CONFIGS[] = {{"with shadows", true, true}, {"no shadows", true, false}};

struct Bench {
    int           config = 0;
    int           phase = -1;
    std::uint32_t frame = 0, began = 0;
    bool          settling = false;
    perf::Stats   results[std::size(PHASES)];
    PopGlass      was = PopGlass::Frozen;
} s_bench;

void bench_step(lv_timer_t *timer)
{
    const std::uint32_t now = lv_tick_get();
    if (s_bench.settling) {
        if (now - s_bench.began < 300) {
            return;
        }
        s_bench.settling = false;
        s_bench.began    = now;
        s_bench.frame    = 0;
        perf::begin();
    }
    const Phase &ph = PHASES[s_bench.phase];
    const std::uint32_t at = now - s_bench.began;
    if (at < ph.ms) {
        ph.tick(at, s_bench.frame++);
        return;
    }
    s_bench.results[s_bench.phase] = perf::end();
    if (std::strncmp(PHASES[s_bench.phase].name, "popover", 7) == 0 && s_bench.config == 0) {
        const std::uint16_t *each = nullptr;
        const int count = perf::frames(&each);
        int n = std::snprintf(s_frames[s_bench.phase % 3], sizeof s_frames[0], "  %s, each frame:", PHASES[s_bench.phase].name);
        for (int i = 0; i < count && n < static_cast<int>(sizeof s_frames[0]) - 6; ++i) {
            n += std::snprintf(s_frames[s_bench.phase % 3] + n, sizeof s_frames[0] - n, " %u", static_cast<unsigned>(each[i]));
        }
        std::snprintf(s_frames[s_bench.phase % 3] + n, sizeof s_frames[0] - n, "\n");
    }
    if (++s_bench.phase == static_cast<int>(std::size(PHASES))) {
        int n = static_cast<int>(std::strlen(s_report));
        n += std::snprintf(s_report + n, sizeof s_report - n, "%s\n", CONFIGS[s_bench.config].name);
        for (std::size_t i = 0; i < std::size(PHASES) && n < static_cast<int>(sizeof s_report); ++i) {
            const perf::Stats &r = s_bench.results[i];
            n += std::snprintf(s_report + n, sizeof s_report - n,
                               "  %-18s %4u frames  %5.1f fps  avg %6.2f ms  worst %6.2f ms\n", PHASES[i].name,
                               static_cast<unsigned>(r.frames), r.span_ms ? r.frames * 1000.0 / r.span_ms : 0.0,
                               static_cast<double>(r.avg_ms), static_cast<double>(r.worst_ms));
        }
        std::printf("%s", s_report);
        if (++s_bench.config < static_cast<int>(std::size(CONFIGS))) {
            set_effects(CONFIGS[s_bench.config].blur, CONFIGS[s_bench.config].shadows);
            s_bench.phase = 0;
            PHASES[0].setup();
            s_bench.settling = true;
            s_bench.began    = now;
            return;
        }
        lv_timer_delete(timer);
        set_effects(true, true);
        set_pop_glass(s_bench.was);
        s_bench.phase = -1;
        show_state();
        return;
    }
    if (PHASES[s_bench.phase].setup != nullptr) {
        PHASES[s_bench.phase].setup();
    }
    s_bench.settling = true;
    s_bench.began    = now;
}

void run_bench()
{
    if (s_bench.phase >= 0) {
        return;
    }
    s_bench.was    = pop_glass();
    s_bench.phase  = 0;
    s_bench.config = 0;
    set_effects(true, true);
    std::snprintf(s_report, sizeof s_report, "kit bench, %s, glass %d%%\n", look() == Look::Dark ? "dark" : "light", glass());
    if (PHASES[0].setup != nullptr) {
        PHASES[0].setup();
    }
    s_bench.settling = true;
    s_bench.began    = lv_tick_get();
    lv_timer_create(bench_step, 16, nullptr);
}

void show_perf(lv_timer_t *)
{
    if (s_perf == nullptr) {
        return;
    }
    const perf::Stats st = perf::last_second();
    char buf[96];
    if (s_bench.phase >= 0) {
        std::snprintf(buf, sizeof buf, "BENCH: %s", PHASES[s_bench.phase].name);
    } else if (st.frames == 0) {
        std::snprintf(buf, sizeof buf, "NOTHING REDRAWN IN THE LAST SECOND");
    } else {
        std::snprintf(buf, sizeof buf, "%u FPS, %.1f MS A FRAME, WORST %.1f", static_cast<unsigned>(st.frames),
                      static_cast<double>(st.avg_ms), static_cast<double>(st.worst_ms));
    }
    lv_label_set_text(s_perf, buf);
}

}  // namespace

void build(perf::Clock clock)
{
    kit::init(Look::Dark);
    perf::start(clock);
    // a screen of its own: whatever else is built stays as it is, behind it
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_screen_load(scr);
    screen(scr);
    lv_obj_t *content = layer(Layer::Content), *chrome = layer(Layer::Chrome);

    lv_obj_t *header = box(content, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_pos(header, CONTENT_X, CONTENT_Y);
    lv_obj_set_size(header, CONTENT_W, 60);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_picker = segmented(header, {"Surfaces", "Controls", "Instruments", "Glass", "Layers"}, 0, 640);
    lv_obj_add_event_cb(
        s_picker, [](lv_event_t *e) { choose_sheet(segmented_chosen(lv_event_get_target_obj(e))); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *col = box(header, LV_FLEX_FLOW_COLUMN, 2);
    s_state = text(col, Role::Small, "", Ink::Secondary);
    s_perf = text(col, Role::Label, "", Ink::Tertiary);
    lv_timer_create(show_perf, 500, nullptr);

    build_surfaces(content);
    build_controls(content);
    build_instruments(content);
    build_glass(content);
    build_layers(content);
    choose_sheet(0);
    build_dock(chrome);
    build_bar(chrome);
    show_state();
}

bool built() { return s_picker != nullptr; }

void bench() { run_bench(); }

int report(char *out, int size)
{
    const int n = std::snprintf(out, static_cast<std::size_t>(size), "%s%s%s%s%s", s_report, s_frames[0], s_frames[1],
                                s_frames[2], kit::freeze_report());
    return std::min(n, size - 1);
}

void key(int code)
{
    switch (code) {
    case '1': case '2': case '3': case '4': case '5':
        choose_sheet(code - '1');
        break;
    case 'f':
        set_pop_glass(pop_glass() == PopGlass::Frozen ? PopGlass::Live : PopGlass::Frozen);
        break;
    case 'm':
        set_pop_motion(pop_motion() == PopMotion::Grow ? PopMotion::Rise : PopMotion::Grow);
        break;
    case 'p':
        run_bench();
        break;
    case 'l':
        set_look(look() == Look::Dark ? Look::Light : Look::Dark);
        break;
    case 'g':
        s_glass_step = (s_glass_step + 1) % static_cast<int>(std::size(GLASS_STEPS));
        set_glass(GLASS_STEPS[s_glass_step]);
        if (s_glass_rail != nullptr) {
            s_glass_rail->set_value(glass(), true);
        }
        break;
    case 'a':
        s_accent = (s_accent + 1) % static_cast<int>(std::size(ACCENTS));
        set_accent(ACCENTS[s_accent]);
        break;
    case 'n':
        notice("HOME ASSISTANT", glyph::local_laundry_service, "Washing machine", "The wash is done", 6000);
        break;
    case 'b':
        s_blur_scrim = !s_blur_scrim;
        break;
    default:
        return;
    }
    show_state();
}

}  // namespace ui::lab
