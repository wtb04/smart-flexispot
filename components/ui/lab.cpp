#include "lab.h"

#include "kit.h"
#include "kit_fonts.h"
#include "lab_lists.h"
#include "lvgl_private.h"  // for the bench, the areas each frame redraws

#if defined(ESP_PLATFORM)
#include "board.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ui::lab {

// lab_pictures.cpp
void build_pictures(lv_obj_t *sheet);
void pictures_shown(bool shown);
void pictures_next_cover();
void pictures_next_track();
void pictures_redraw_mode(int mode);
void pictures_redraw();
void pictures_bench_done();
void pictures_measure_corners();

// lab_pages.cpp
void build_pages(lv_obj_t *const items[4], void (*shown)(int page), void (*back)());

namespace {

using namespace ui::kit;
namespace ic = ui::kit::glyph;

constexpr std::int32_t CONTENT_X = 142, CONTENT_Y = 104, CONTENT_W = 1122, CONTENT_H = 600;
constexpr std::int32_t SHEET_Y = 76, SHEET_H = CONTENT_H - SHEET_Y;
constexpr std::uint32_t ACCENTS[] = {0, 0x5aa9ff, 0x7ccf6a, 0xb48cff, 0xff6f8e, 0x3fd0c0};
constexpr int GLASS_STEPS[] = {0, 35, 50, 70, 100};

constexpr int PAGES_SHEET = 7;  // the kit's own pages, in place of a sheet
lv_obj_t *s_sheets[PAGES_SHEET]{};
lv_obj_t *s_header = nullptr;
lv_obj_t *s_items[4]{};
bool      s_in_pages   = false;
int       s_last_sheet = 0;
lv_obj_t *s_perf = nullptr;
lv_obj_t *s_light_item = nullptr, *s_bench_btn = nullptr, *s_bench_seg = nullptr, *s_slab = nullptr;
lv_obj_t *s_picker = nullptr;
lv_obj_t *s_state  = nullptr;
int       s_accent = 0;
int       s_glass_step = 2;
bool      s_blur_scrim = true;

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

// The sheets out of the way as a page comes to show, the dock's or the picker's.
void pages_shown(int)
{
    if (s_in_pages) {
        return;
    }
    s_in_pages = true;
    lv_obj_set_hidden(s_header, true);
    for (lv_obj_t *sheet : s_sheets) {
        if (sheet != nullptr) {
            lv_obj_set_hidden(sheet, true);
        }
    }
}

void choose_sheet(int index)
{
    lv_obj_scroll_to_y(layer(Layer::Content), 0, LV_ANIM_OFF);
    if (index == PAGES_SHEET) {
        pictures_shown(false);
        show_page(std::max(shown_page(), 0), false);
        return;
    }
    if (s_in_pages) {
        s_in_pages = false;
        hide_pages();
        lv_obj_set_hidden(s_header, false);
    }
    s_last_sheet = index;
    for (int i = 0; i < PAGES_SHEET; ++i) {
        if (s_sheets[i] != nullptr) {
            if (i == index) {
                lv_obj_remove_flag(s_sheets[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_sheets[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    pictures_shown(index == 5);
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

// A play button that plays and pauses, so a tap on it is never a dead one.
void toggles_play(lv_obj_t *btn)
{
    lv_obj_add_event_cb(
        btn,
        [](lv_event_t *e) {
            lv_obj_t *glyph = lv_obj_get_child(lv_event_get_current_target_obj(e), 0);
            const bool playing = std::strcmp(lv_label_get_text(glyph), ic::pause) == 0;
            set_icon(glyph, playing ? ic::play_arrow : ic::pause, false);
        },
        LV_EVENT_CLICKED, nullptr);
}

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
    button(row, Button::Raised, ic::power_settings_new, "All off");
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
    lv_obj_t *body = popover(lv_event_get_current_target_obj(e), 440, 420, From::Below, s_blur_scrim);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, space::l, 0);
    head(body, "Heating", "The room is at 20.5°");
    static int target = 210;
    lv_obj_t *read = text(body, Role::ReadXXL, "");
    lv_label_set_text_fmt(read, "%d.%d°", target / 10, target % 10);
    lv_obj_t *row = box(body, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(row, LV_PCT(100));
    for (int dir : {-1, 1}) {
        lv_obj_t *b = button(row, Button::Raised, dir < 0 ? ic::remove : ic::add, nullptr);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, size::button_lg);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *ev) {
                const int step = lv_obj_get_index(lv_event_get_current_target_obj(ev)) == 0 ? -5 : 5;
                target = std::clamp(target + step, 150, 250);
                lv_label_set_text_fmt(static_cast<lv_obj_t *>(lv_event_get_user_data(ev)), "%d.%d°", target / 10, target % 10);
            },
            LV_EVENT_CLICKED, read);
    }
    segmented(body, {"Heat", "Off"}, 0, 392);
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
    toggles_play(pause);
    lv_obj_t *next = button(row, Button::Raised, ic::skip_next, nullptr, true);
    lv_obj_set_size(next, 72, 72);
}

// --- chrome ---------------------------------------------------------------

void build_dock(lv_obj_t *scr)
{
    lv_obj_t *dock = glass(scr, Glass::Dock);
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
    constexpr std::int32_t PAD = 20;  // what is placed by hand counts from inside the padding
    lv_obj_set_style_pad_left(height, PAD, 0);
    lv_obj_t *w = well(height);
    lv_obj_add_flag(w, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(w, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(w, 88, 104);
    lv_obj_set_pos(w, -PAD, 0);
    lv_obj_set_style_radius(w, 24, 0);
    // the desk's travel as a ruler down the side, its mark where the desk stands
    constexpr std::int32_t RULE_TOP = 14, RULE_LEN = 76;
    lv_obj_t *rule = lv_scale_create(height);
    lv_obj_add_flag(rule, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    lv_scale_set_mode(rule, LV_SCALE_MODE_VERTICAL_RIGHT);
    lv_scale_set_total_tick_count(rule, 14);
    lv_scale_set_major_tick_every(rule, 0);
    lv_scale_set_label_show(rule, false);
    lv_obj_set_size(rule, 1, RULE_LEN);
    lv_obj_set_pos(rule, 9 - PAD, RULE_TOP);
    lv_obj_set_style_line_opa(rule, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_length(rule, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_width(rule, 1, LV_PART_ITEMS);
    lv_obj_t *mark = lv_obj_create(height);
    lv_obj_remove_style_all(mark);
    lv_obj_add_flag(mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mark, 12, 3);
    lv_obj_set_style_radius(mark, 2, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_pos(mark, 6 - PAD, RULE_TOP + RULE_LEN - 1 - (740 - 660) * (RULE_LEN - 1) / (1310 - 660));
    static lv_obj_t *s_rule[2];
    s_rule[0] = rule, s_rule[1] = mark;
    const auto paint_rule = [](void *ctx) {
        auto **r = static_cast<lv_obj_t **>(ctx);
        lv_obj_set_style_line_color(r[0], colour(Ink::Primary), LV_PART_ITEMS);
        lv_obj_set_style_line_opa(r[0], colours().tick_opa, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(r[1], colour(Ink::Accent), 0);
    };
    paint_rule(s_rule);
    on_restyle(rule, paint_rule, s_rule);
    lv_obj_t *read = text(height, Role::ReadS, "74.0");
    lv_obj_add_flag(read, LV_OBJ_FLAG_EVENT_BUBBLE);
    text(height, Role::Label, "CM", Ink::Tertiary);
    lv_obj_add_event_cb(height, open_desk, LV_EVENT_CLICKED, nullptr);
    popover_source(height);

    lv_obj_t *stand = button(dock, Button::Raised, nullptr, nullptr);
    lv_obj_set_size(stand, 88, 80);
    lv_obj_set_style_radius(stand, 24, 0);
    lv_obj_t *col = box(stand, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icon(col, ic::arrow_upward, 24, false);
    icon(col, ic::desk, 34, false);

    spacer(dock);
    const char *glyphs[4] = {ic::home, ic::calendar_month, ic::flight, ic::timer};
    for (int i = 0; i < 4; ++i) {
        if (i == 1) {
            spacer(dock);
        }
        if (i == 3) {
            hairline(dock, false, 40);
        }
        s_items[i] = dock_item(dock, glyphs[i]);
    }
}

void build_bar(lv_obj_t *scr)
{
    lv_obj_t *bar = box(scr, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_pos(bar, CONTENT_X, 16);
    lv_obj_set_size(bar, CONTENT_W, size::bar);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    set_status(status(bar), "9:41", true, true);
    spacer(bar);

    lv_obj_t *room  = capsule(bar);
    lv_obj_t *light = s_light_item = capsule_item(room, ic::lightbulb, "2", 118);
    lv_obj_set_style_text_font(lv_obj_get_child(light, 0), fonts::icon(28, true), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(light, 0), lv_color_hex(colours().lamp), 0);
    lv_obj_add_event_cb(light, open_lights, LV_EVENT_CLICKED, nullptr);
    popover_source(light);
    capsule_divider(room);
    lv_obj_t *heat = capsule_item(room, nullptr, "20.5°", 150);
    lv_obj_move_to_index(dot(heat, Ink::Accent), 0);  // heating; warm enough is amber, off is grey
    lv_obj_add_event_cb(heat, open_heating, LV_EVENT_CLICKED, nullptr);
    popover_source(heat);

    lv_obj_t *media = capsule(bar);
    lv_obj_set_width(media, 452);
    lv_obj_set_style_pad_column(media, space::m, 0);
    lv_obj_add_flag(media, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(media, colours().glass_solid_opa, LV_STATE_PRESSED);  // the glass firms up under the finger
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
    toggles_play(pp);
    lv_obj_add_event_cb(media, open_media, LV_EVENT_CLICKED, nullptr);
    popover_source(media);
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
    text(holder, Role::Label, "INSIDE A TILE", Ink::Secondary);
    lv_obj_t *w = well(holder);
    lv_obj_set_size(w, LV_PCT(100), 100);
    lv_obj_align(text(w, Role::ReadS, "74.0"), LV_ALIGN_LEFT_MID, space::s, 0);
    lv_obj_align(text(w, Role::Small, "Flat, a shade of the ink", Ink::Secondary), LV_ALIGN_RIGHT_MID, -space::s, 0);

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
    toggles_play(r2);
    lv_obj_t *r3 = button(wrap, Button::Raised, ic::restart_alt, nullptr, true);
    lv_obj_set_width(r3, 56);
    button(wrap, Button::Raised, ic::bedtime, "Tap to turn on", false, true);
    lv_obj_t *no = button(wrap, Button::Raised, nullptr, "Unusable");
    set_usable(no, false);
    lv_obj_t *hint = text(buttons, Role::Small, "Press and hold any of them: they give a little and spring back.", Ink::Tertiary);
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
        lv_obj_set_size(row, LV_PCT(100), 60);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *label = text(row, Role::Body, name);
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
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

const Zone CO2_ZONES[] = {{400, 800, Ink::Green, "Good"}, {800, 1200, Ink::Amber, "Fair"}, {1200, 2000, Ink::Red, "Poor"}};
const Zone VOC_ZONES[] = {{0, 333, Ink::Green, "Good"}, {333, 666, Ink::Amber, "Fair"}, {666, 1000, Ink::Red, "Poor"}};
const Zone HUM_ZONES[] = {{0, 30, Ink::Red, "Very dry"}, {30, 40, Ink::Amber, "Dry"}, {40, 60, Ink::Green, "Good"},
                          {60, 70, Ink::Amber, "Damp"}, {70, 100, Ink::Red, "Very damp"}};
const Zone PM_ZONES[]  = {{0, 12, Ink::Green, "Good"}, {12, 35, Ink::Amber, "Fair"}, {35, 75, Ink::Red, "Poor"}};

Reading  *s_readings[4]{};
lv_obj_t *s_room = nullptr;
lv_obj_t *s_room_line = nullptr, *s_focus_fill = nullptr;

void wander_readings(bool animate)
{
    const std::int32_t values[4] = {500 + std::rand() % 1100, 40 + std::rand() % 500, 25 + std::rand() % 55,
                                    std::rand() % 40};
    for (int i = 0; i < 4; ++i) {
        if (s_readings[i] != nullptr) {
            s_readings[i]->set_value(values[i], animate);
        }
    }
}

void build_instruments(lv_obj_t *scr)
{
    lv_obj_t *s = s_sheets[2] = sheet(scr);
    constexpr std::int32_t AIR_W = 268, HALF = (SHEET_H - space::l) / 2;
    lv_obj_t *air = box(s, LV_FLEX_FLOW_ROW_WRAP, space::l);
    lv_obj_set_size(air, 2 * AIR_W + space::l, SHEET_H);
    struct R { const char *caption, *unit; std::int32_t min, max; const Zone *zones; int n; const char *note; std::int32_t v; };
    const R specs[4] = {{"CO2", "ppm", 400, 2000, CO2_ZONES, 3, "Good up to 800", 640},
                        {"VOC", "ppb", 0, 1000, VOC_ZONES, 3, "Good up to 333", 120},
                        {"Humidity", "%", 0, 100, HUM_ZONES, 5, "Good from 40 to 60", 48},
                        {"PM2.5", "µg/m³", 0, 75, PM_ZONES, 3, "Good up to 12", 4}};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *t = tile(air, Lift::Low);
        lv_obj_set_size(t, AIR_W, HALF);
        ReadingSpec rs;
        rs.caption = specs[i].caption, rs.unit = specs[i].unit, rs.min = specs[i].min, rs.max = specs[i].max;
        rs.zones = specs[i].zones, rs.zone_count = specs[i].n, rs.note = specs[i].note;
        s_readings[i] = new Reading(t, rs);
        s_readings[i]->set_value(specs[i].v, false);
    }

    lv_obj_t *right = box(s, LV_FLEX_FLOW_COLUMN, space::l);
    lv_obj_set_size(right, CONTENT_W - 2 * AIR_W - 2 * space::l, SHEET_H);

    lv_obj_t *heat = tile(right, Lift::Low);
    lv_obj_set_size(heat, LV_PCT(100), HALF);
    lv_obj_set_flex_flow(heat, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *title = box(heat, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_flex_align(title, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    dot(title, Ink::Accent);
    text(title, Role::H3, "Heating");
    spacer(heat);
    lv_obj_t *row = box(heat, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *now = box(row, LV_FLEX_FLOW_COLUMN, 0);
    s_room = text(now, Role::ReadXL, "20.5°");
    s_room_line = text(now, Role::Body, "Heating to 21.0°", Ink::Secondary);
    StepperSpec target;
    target.min = 150, target.max = 250, target.step = 5, target.value = 210, target.decimals = 1, target.unit = "°";
    auto *stepper = new Stepper(row, target);
    own(stepper->obj(), stepper);
    lv_obj_add_event_cb(
        stepper->obj(),
        [](lv_event_t *e) {
            auto *st = static_cast<Stepper *>(lv_event_get_user_data(e));
            char line[32];
            std::snprintf(line, sizeof line, "Heating to %d.%d°", static_cast<int>(st->value() / 10),
                          static_cast<int>(st->value() % 10));
            lv_label_set_text(s_room_line, line);
        },
        LV_EVENT_VALUE_CHANGED, stepper);

    lv_obj_t *focus = tile(right, Lift::Low);
    lv_obj_set_size(focus, LV_PCT(100), HALF);
    lv_obj_set_flex_flow(focus, LV_FLEX_FLOW_COLUMN);
    text(focus, Role::H3, "Focus");
    spacer(focus);
    text(focus, Role::ReadXL, "24:51");
    text(focus, Role::Body, "Until 10:06", Ink::Secondary);
    lv_obj_t *track = well(focus);
    lv_obj_set_size(track, LV_PCT(100), 6);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_margin_top(track, space::m, 0);
    s_focus_fill = lv_obj_create(track);
    lv_obj_remove_style_all(s_focus_fill);
    lv_obj_set_size(s_focus_fill, LV_PCT(4), LV_PCT(100));
    lv_obj_set_style_radius(s_focus_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_focus_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_focus_fill, colour(Ink::Accent), 0);
    on_restyle(s_focus_fill, [](void *o) { lv_obj_set_style_bg_color(static_cast<lv_obj_t *>(o), colour(Ink::Accent), 0); },
               s_focus_fill);

    lv_timer_create(
        [](lv_timer_t *) {
            wander_readings(true);
            if (s_room != nullptr) {
                const int room = 195 + std::rand() % 25;
                lv_label_set_text_fmt(s_room, "%d.%d°", room / 10, room % 10);
            }
            if (s_focus_fill != nullptr) {
                s_focus_lit = s_focus_lit % 25 + 1;
                lv_obj_set_width(s_focus_fill, lv_pct(s_focus_lit * 4));
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
    lv_obj_add_event_cb(slab, [](lv_event_t *e) { motion::moving(lv_event_get_target_obj(e), true); }, LV_EVENT_PRESSED, nullptr);
    for (lv_event_code_t up : {LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(slab, [](lv_event_t *e) { motion::moving(lv_event_get_target_obj(e), false); }, up, nullptr);
    }
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

void open_tick(std::uint32_t at, std::uint32_t frame)
{
    if (frame == 0) {
        prepare_popover();  // the finger lands
    } else if (at >= 120 && at < 136 && !popover_open()) {
        open_lights_from(s_light_item);  // and lifts, a tap's length later; left open for the next phase
    }
}

void close_tick(std::uint32_t, std::uint32_t frame)
{
    if (frame == 0) {
        close_popover(true);
    }
}

// From the agenda back to Home, its tiles and its dial the most a page change draws.
template <Change How> void page_setup()
{
    choose_sheet(PAGES_SHEET);
    set_change(How);
    show_page(1, false);
}

void page_tick(std::uint32_t, std::uint32_t frame)
{
    if (frame == 0) {
        show_page(0);
    }
}

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
    {"readings", 2000, [] { choose_sheet(2); },
     [](std::uint32_t at, std::uint32_t) {
         if (at % 500 < 16) {
             wander_readings(true);
         }
     }},
    {"popover, live", 1300, [] { choose_sheet(0); set_pop_glass(PopGlass::Live); },
     [](std::uint32_t at, std::uint32_t frame) {
         if (frame == 0) {
             prepare_popover();  // the finger lands
         } else if (at >= 120 && at < 136) {
             open_lights_from(s_light_item);  // and lifts, a tap's length later
         } else if (at >= 900 && popover_open()) {
             close_popover(true);
         }
     }},
    {"popover grow, open", 900, [] { set_pop_glass(PopGlass::Frozen); set_pop_motion(PopMotion::Grow); }, open_tick},
    {"popover grow, close", 700, nullptr, close_tick},
    {"popover rise, open", 900, [] { set_pop_motion(PopMotion::Rise); }, open_tick},
    {"popover rise, close", 700, nullptr, close_tick},
    {"drag glass", 1000, [] { set_pop_motion(PopMotion::Grow); choose_sheet(3); },
     [](std::uint32_t at, std::uint32_t frame) {
         static bool held = false;  // as a finger would hold it
         if (frame == 0 || (held && at >= 960)) {
             held = frame == 0;
             motion::moving(s_slab, held);
         }
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
    {"cover crossfade", 1600, [] { choose_sheet(5); pictures_redraw_mode(-1); },
     [](std::uint32_t at, std::uint32_t) {
         if (at % 400 < 16) {
             pictures_next_cover();
         }
     }},
    {"backdrop", 1600, nullptr,
     [](std::uint32_t at, std::uint32_t) {
         if (at % 800 < 16) {
             pictures_next_track();
         }
     }},
    {"corners, kit cover", 500, nullptr, [](std::uint32_t, std::uint32_t) { pictures_redraw(); }},
    {"corners, baked", 500, [] { pictures_redraw_mode(1); }, [](std::uint32_t, std::uint32_t) { pictures_redraw(); }},
    {"corners, all alpha", 500, [] { pictures_redraw_mode(2); }, [](std::uint32_t, std::uint32_t) { pictures_redraw(); }},
    {"corners, image radius", 500, [] { pictures_redraw_mode(3); }, [](std::uint32_t, std::uint32_t) { pictures_redraw(); }},
    {"corners, clip corner", 500, [] { pictures_redraw_mode(4); }, [](std::uint32_t, std::uint32_t) { pictures_redraw(); }},
    {"page, instant", 900, page_setup<Change::Instant>, page_tick},
    {"page, wipe", 900, page_setup<Change::Wipe>, page_tick},
    {"page, spread", 900, page_setup<Change::Spread>, page_tick},
    {"page, stagger", 900, page_setup<Change::Stagger>, page_tick},
    {"page, slide", 900, page_setup<Change::Slide>, page_tick},
    {"page, fade", 900, page_setup<Change::Fade>, page_tick},
    {"list scroll", 1600, [] { choose_sheet(6); }, lists::scroll_tick},
    {"row press", 1400, nullptr, lists::press_tick},
    {"readouts", 1600, nullptr, lists::readouts_tick},
    {"sheet open", 900, nullptr, lists::sheet_open_tick},
    {"sheet scroll", 1400, nullptr, lists::sheet_scroll_tick},
    {"sheet close", 700, nullptr, lists::sheet_close_tick},
};
constexpr std::size_t PHASE_COUNT = std::size(PHASES);

// a popover or a sheet: its backdrop wants longer to be made ahead
bool pops(const char *name) { return std::strncmp(name, "popover", 7) == 0 || std::strncmp(name, "sheet", 5) == 0; }

KIT_IN_PSRAM char s_report[10000];
KIT_IN_PSRAM char s_frames[14][480];
int  s_frame_lines = 0;

// What each frame redraws, as LVGL has it once it has joined the areas: the
// panel's cost goes with it, drawing and turning both.
struct Drawn {
    std::uint32_t frames = 0, worst = 0;
    std::uint64_t px = 0;
};
Drawn         s_drawn, s_drawn_of[2][PHASE_COUNT];
bool          s_drawing = false;
std::uint32_t s_drawn_each[48]{};

void on_render(lv_event_t *e)
{
    if (!s_drawing) {
        return;
    }
    const auto *disp = static_cast<const lv_display_t *>(lv_event_get_current_target(e));
    std::uint32_t px = 0;
    for (std::uint32_t i = 0; i < disp->inv_p; ++i) {
        if (!disp->inv_area_joined[i]) {
            px += lv_area_get_size(&disp->inv_areas[i]);
        }
    }
    if (s_drawn.frames < std::size(s_drawn_each)) {
        s_drawn_each[s_drawn.frames] = px;
    }
    ++s_drawn.frames;
    s_drawn.px += px;
    s_drawn.worst = std::max(s_drawn.worst, px);
}
#if defined(ESP_PLATFORM)
KIT_IN_PSRAM board::FlushTimes s_flush[2][PHASE_COUNT];
#endif

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
    perf::Stats   results[PHASE_COUNT];
    PopGlass      was = PopGlass::Frozen;
    Change        change = Change::Wipe;
} s_bench;

void bench_step(lv_timer_t *timer)
{
    const std::uint32_t now = lv_tick_get();
    if (s_bench.settling) {
        const bool popover_next = pops(PHASES[s_bench.phase].name);
        const bool page_next    = std::strncmp(PHASES[s_bench.phase].name, "page", 4) == 0;  // its shadows painted in
        if (now - s_bench.began < (popover_next ? 900u : page_next ? 700u : 300u)) {
            return;
        }
        s_bench.settling = false;
        s_bench.began    = now;
        s_bench.frame    = 0;
#if defined(ESP_PLATFORM)
        board::take_flush_times();
#endif
        perf::begin();
        s_drawn   = Drawn{};
        s_drawing = true;
    }
    const Phase &ph = PHASES[s_bench.phase];
    const std::uint32_t at = now - s_bench.began;
    if (at < ph.ms) {
        ph.tick(at, s_bench.frame++);
        return;
    }
    s_bench.results[s_bench.phase] = perf::end();
    s_drawing = false;
    s_drawn_of[s_bench.config][s_bench.phase] = s_drawn;
#if defined(ESP_PLATFORM)
    s_flush[s_bench.config][s_bench.phase] = board::take_flush_times();
#endif
    const bool paged  = std::strncmp(PHASES[s_bench.phase].name, "page", 4) == 0;
    const bool traced = pops(PHASES[s_bench.phase].name) ||
                        std::strcmp(PHASES[s_bench.phase].name, "notice") == 0 || paged;
    if (traced && s_bench.config == 0 && s_frame_lines < static_cast<int>(std::size(s_frames))) {
        const std::uint16_t *each = nullptr;
        const int count = std::min<int>(perf::frames(&each), static_cast<int>(std::min<std::uint32_t>(s_drawn.frames, 48)));
        char *line = s_frames[s_frame_lines++];
        const int size = static_cast<int>(sizeof s_frames[0]);
        int n = std::snprintf(line, size, "  %s, each frame in ms/kpx:", PHASES[s_bench.phase].name);
        for (int i = 0; i < count && n < size - 12; ++i) {
            n += std::snprintf(line + n, size - n, " %u/%u", static_cast<unsigned>(each[i]),
                               static_cast<unsigned>((s_drawn_each[i] + 500) / 1000));
        }
        std::snprintf(line + n, size - n, paged ? "\n    %s\n" : "\n", change_report());
    }
    if (++s_bench.phase == static_cast<int>(std::size(PHASES))) {
        int n = static_cast<int>(std::strlen(s_report));
        n += std::snprintf(s_report + n, sizeof s_report - n, "%s\n", CONFIGS[s_bench.config].name);
        for (std::size_t i = 0; i < std::size(PHASES) && n < static_cast<int>(sizeof s_report); ++i) {
            const perf::Stats &r = s_bench.results[i];
            n += std::snprintf(s_report + n, sizeof s_report - n,
                               "  %-20s %4u frames  %5.1f fps  avg %6.2f ms  worst %6.2f ms", PHASES[i].name,
                               static_cast<unsigned>(r.frames), r.span_ms ? r.frames * 1000.0 / r.span_ms : 0.0,
                               static_cast<double>(r.avg_ms), static_cast<double>(r.worst_ms));
            const Drawn &d = s_drawn_of[s_bench.config][i];
            n += std::snprintf(s_report + n, sizeof s_report - n, "  redrawn avg %4u kpx  worst %4u kpx  all %6u kpx",
                               static_cast<unsigned>(d.frames ? d.px / d.frames / 1000 : 0),
                               static_cast<unsigned>(d.worst / 1000), static_cast<unsigned>(d.px / 1000));
#if defined(ESP_PLATFORM)
            const board::FlushTimes &f = s_flush[s_bench.config][i];
            const double per = f.frames ? 1000.0 * f.frames : 1.0;
            n += std::snprintf(s_report + n, sizeof s_report - n, "  | a frame: turn %.1f, wait %.1f, catch up %.1f ms, %llu px",
                               f.rotate_us / per, f.wait_us / per, f.catch_up_us / per,
                               static_cast<unsigned long long>(f.frames ? f.pixels / f.frames : 0));
#endif
            n += std::snprintf(s_report + n, sizeof s_report - n, "\n");
        }
        std::printf("%s", s_report);
        if (s_bench.config == 0) {
            for (int i = 0; i < s_frame_lines; ++i) {
                std::printf("%s", s_frames[i]);
            }
            std::printf("%s", kit::freeze_report());
            std::printf("%s", kit::pictures_report());
        }
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
        pictures_bench_done();
        set_change(s_bench.change);
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
    s_bench.change = change();
    s_bench.phase  = 0;
    s_bench.config = 0;
    set_effects(true, true);
    std::snprintf(s_report, sizeof s_report, "kit bench, %s, glass %d%%\n", look() == Look::Dark ? "dark" : "light", glass());
    s_frame_lines = 0;
    static bool watching = false;
    if (!watching) {
        watching = true;
        lv_display_add_event_cb(lv_display_get_default(), on_render, LV_EVENT_RENDER_START, nullptr);
    }
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

    lv_obj_t *header = s_header = box(content, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_pos(header, CONTENT_X, CONTENT_Y);
    lv_obj_set_size(header, CONTENT_W, 60);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_picker = segmented(header, {"Surfaces", "Controls", "Readings", "Glass", "Layers", "Pictures", "Lists", "Pages"}, 0, 860);
    lv_obj_add_event_cb(
        s_picker, [](lv_event_t *e) { choose_sheet(segmented_chosen(lv_event_get_target_obj(e))); },
        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *col = box(header, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_flex_grow(col, 1);
    s_state = text(col, Role::Small, "", Ink::Secondary);
    s_perf = text(col, Role::Label, "", Ink::Tertiary);
    for (lv_obj_t *line : {s_state, s_perf}) {
        lv_obj_set_size(line, LV_PCT(100), lv_font_get_line_height(lv_obj_get_style_text_font(line, LV_PART_MAIN)));
        lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_timer_create(show_perf, 500, nullptr);

    build_surfaces(content);
    build_controls(content);
    build_instruments(content);
    build_glass(content);
    build_layers(content);
    build_pictures(s_sheets[5] = sheet(content));
    lists::build(s_sheets[6] = sheet(content));
    choose_sheet(0);
    build_dock(chrome);
    build_bar(chrome);
    build_pages(s_items, pages_shown, [] { choose_sheet(s_last_sheet); });
    show_state();
}

bool built() { return s_picker != nullptr; }

void bench() { run_bench(); }

int report(char *out, int size)
{
    int n = std::snprintf(out, static_cast<std::size_t>(size), "%s", s_report[0] != 0 ? s_report : "no bench run yet\n");
    for (int i = 0; i < s_frame_lines && n < size; ++i) {
        n += std::snprintf(out + n, static_cast<std::size_t>(size - n), "%s", s_frames[i]);
    }
    if (n < size) {
        n += std::snprintf(out + n, static_cast<std::size_t>(size - n), "%s", kit::freeze_report());
    }
    if (n < size) {
        n += std::snprintf(out + n, static_cast<std::size_t>(size - n), "%s", kit::pictures_report());
    }
    if (n < size) {
        n += std::snprintf(out + n, static_cast<std::size_t>(size - n), "%s", lists::report());
    }
    return std::min(n, size - 1);
}

void key(int code)
{
    switch (code) {
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
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
    case 'o':
        open_lights_from(s_light_item);
        break;
    case 'c':
        close_popover(true);
        break;
    case 'n':
        notice("HOME ASSISTANT", glyph::local_laundry_service, "Washing machine", "The wash is done", 6000);
        break;
    case 'b':
        s_blur_scrim = !s_blur_scrim;
        break;
    case 'k':
        pictures_measure_corners();
        break;
    case 't': {  // a finger on the next of the dock's items, lifted a tap's length later
        static int next = 0;
        next = (next + 1) % 4;
        lv_obj_t *item = s_items[next];
        lv_obj_add_state(item, LV_STATE_PRESSED);
        lv_obj_send_event(item, LV_EVENT_PRESSED, nullptr);
        lv_timer_t *lift = lv_timer_create(
            [](lv_timer_t *t) {
                auto *it = static_cast<lv_obj_t *>(lv_timer_get_user_data(t));
                lv_obj_remove_state(it, LV_STATE_PRESSED);
                lv_obj_send_event(it, LV_EVENT_RELEASED, nullptr);
                lv_obj_send_event(it, LV_EVENT_CLICKED, nullptr);
            },
            120, item);
        lv_timer_set_repeat_count(lift, 1);
        break;
    }
    case 'v':
        choose_sheet(6);
        lists::study();
        break;
    case 'j':
        lists::nudge();
        break;
    case 'h':
        lists::show_press();
        break;
    default:
        return;
    }
    show_state();
}

}  // namespace ui::lab
