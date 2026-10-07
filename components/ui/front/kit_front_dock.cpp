// The dock: the desk's height over its travel, the shortcut between standing
// and sitting, the items that choose the pages, and the focus timer; the desk
// folds out of its height as a popover.

#include "kit_front.h"

#include "focus_model.h"
#include "kit_fonts.h"
#include "room_model.h"
#include "topics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#ifndef CONFIG_LOCTEK_MIN_HEIGHT_MM  // the simulator's desk
#define CONFIG_LOCTEK_MIN_HEIGHT_MM 660
#define CONFIG_LOCTEK_MAX_HEIGHT_MM 1310
#endif

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;

constexpr std::int32_t ITEM_W = 88, RULE_TOP = 14, RULE_LEN = 76, PAD = 20;
constexpr int TRAVEL_MIN = CONFIG_LOCTEK_MIN_HEIGHT_MM, TRAVEL_MAX = CONFIG_LOCTEK_MAX_HEIGHT_MM;

lv_obj_t *s_height = nullptr, *s_height_read = nullptr, *s_mark = nullptr;
lv_obj_t *s_shortcut = nullptr, *s_shortcut_arrow = nullptr, *s_shortcut_desk = nullptr;
lv_obj_t *s_focus = nullptr, *s_focus_glyph = nullptr, *s_focus_left = nullptr, *s_focus_rule = nullptr;

std::int32_t mark_y(int mm)
{
    const int at = std::clamp(mm, TRAVEL_MIN, TRAVEL_MAX) - TRAVEL_MIN;
    return RULE_TOP + RULE_LEN - 1 - at * (RULE_LEN - 1) / (TRAVEL_MAX - TRAVEL_MIN);
}

// Where the shortcut goes: standing from Sit, sitting from anywhere else, and
// on its way to either, that one, so a tap stops it as the box's keys do.
int shortcut_target()
{
    const detail::DeskState &desk = detail::desk_state();
    if (desk.travelling == detail::STAND_PRESET || desk.travelling == detail::SIT_PRESET) {
        return desk.travelling;
    }
    return desk.preset_active[detail::SIT_PRESET] ? detail::STAND_PRESET : detail::SIT_PRESET;
}

void paint_desk()
{
    const detail::DeskState &desk = detail::desk_state();
    char buf[24] = "--";
    if (desk.height_mm >= 0) {
        std::snprintf(buf, sizeof buf, "%d.%d", desk.height_mm / 10, desk.height_mm % 10);
    }
    if (std::strcmp(lv_label_get_text(s_height_read), buf) != 0) {
        lv_label_set_text(s_height_read, buf);
    }
    lv_obj_set_hidden(s_mark, desk.height_mm < 0);
    if (desk.height_mm >= 0 && lv_obj_get_y(s_mark) != mark_y(desk.height_mm)) {
        lv_obj_set_y(s_mark, mark_y(desk.height_mm));
    }
    // which way it goes, or which way it moves by other keys; tinted while it is on its way there
    const int  target = shortcut_target();
    const bool going  = desk.travelling == target;
    const bool up     = going || desk.moving == 0 ? target == detail::STAND_PRESET : desk.moving > 0;
    set_icon(s_shortcut_arrow, up ? ic::arrow_upward : ic::arrow_downward, false);
    static int s_shown = -1;
    const int  look    = going ? 2 : desk.moving != 0 ? 1 : 0;
    if (std::exchange(s_shown, look) != look) {
        set_soft(s_shortcut, going);
        set_ink(s_shortcut_arrow, look > 0 ? Ink::Accent : Ink::Secondary);
        set_ink(s_shortcut_desk, going ? Ink::Accent : Ink::Primary);
    }
    for (lv_obj_t *control : {s_height, s_shortcut}) {
        set_usable(control, desk.available);
    }
}

void paint_focus()
{
    const Focus &focus = detail::focus_state();
    const bool   idle  = detail::focus_idle(focus);
    lv_obj_set_hidden(s_focus_glyph, !idle);
    lv_obj_set_hidden(s_focus_left, idle);
    if (!idle) {
        char buf[16];
        detail::focus_clock_text(focus, buf, sizeof buf);
        if (std::strcmp(lv_label_get_text(s_focus_left), buf) != 0) {
            lv_label_set_text(s_focus_left, buf);
        }
        static int s_ink = -1;
        const int  ink   = detail::focus_resting(focus) ? 1 : 0;
        if (std::exchange(s_ink, ink) != ink) {
            set_ink(s_focus_left, ink == 1 ? Ink::Green : Ink::Accent);
        }
        const lv_opa_t opa = detail::focus_paused(focus) ? LV_OPA_40 : LV_OPA_COVER;
        if (lv_obj_get_style_opa(s_focus_left, LV_PART_MAIN) != opa) {
            lv_obj_set_style_opa(s_focus_left, opa, 0);
        }
    }
    for (lv_obj_t *part : {s_focus, s_focus_rule}) {
        if (lv_obj_is_hidden(part) != owner_away()) {
            lv_obj_set_hidden(part, owner_away());
        }
    }
}

void build_height(lv_obj_t *dock)
{
    s_height = button(dock, Button::Flat, nullptr, nullptr);
    lv_obj_set_size(s_height, ITEM_W, 104);
    lv_obj_set_style_pad_all(s_height, 0, 0);
    lv_obj_set_style_pad_left(s_height, PAD, 0);
    lv_obj_set_style_pad_row(s_height, 2, 0);
    lv_obj_set_flex_flow(s_height, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_height, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *back = well(s_height);
    lv_obj_add_flag(back, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(back, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(back, ITEM_W, 104);
    lv_obj_set_pos(back, -PAD, 0);
    lv_obj_set_style_radius(back, 24, 0);
    // the desk's travel as a ruler down the side, its mark where the desk stands
    lv_obj_t *rule = lv_scale_create(s_height);
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
    s_mark = lv_obj_create(s_height);
    lv_obj_remove_style_all(s_mark);
    lv_obj_add_flag(s_mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(s_mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_mark, 12, 3);
    lv_obj_set_style_radius(s_mark, 2, 0);
    lv_obj_set_style_bg_opa(s_mark, LV_OPA_COVER, 0);
    lv_obj_set_x(s_mark, 6 - PAD);
    static lv_obj_t *s_rule[2];
    s_rule[0] = rule, s_rule[1] = s_mark;
    const auto paint_rule = [](void *ctx) {
        auto **r = static_cast<lv_obj_t **>(ctx);
        lv_obj_set_style_line_color(r[0], colour(Ink::Primary), LV_PART_ITEMS);
        lv_obj_set_style_line_opa(r[0], colours().tick_opa, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(r[1], colour(Ink::Accent), 0);
    };
    paint_rule(s_rule);
    on_restyle(rule, paint_rule, s_rule);
    s_height_read = text(s_height, Role::ReadS, "--");
    lv_obj_add_flag(s_height_read, LV_OBJ_FLAG_EVENT_BUBBLE);
    text(s_height, Role::Label, "CM", Ink::Tertiary);
    lv_obj_add_event_cb(s_height, [](lv_event_t *e) { open_desk(lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);
    popover_source(s_height);
}

void build_shortcut(lv_obj_t *dock)
{
    s_shortcut = button(dock, Button::Raised, nullptr, nullptr);
    lv_obj_set_size(s_shortcut, ITEM_W, 80);
    lv_obj_set_style_radius(s_shortcut, 24, 0);
    lv_obj_t *col = flow(s_shortcut, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_shortcut_arrow = icon(col, ic::arrow_upward, 24, false);
    s_shortcut_desk  = icon(col, ic::desk, 34, false);
    lv_obj_add_event_cb(s_shortcut, [](lv_event_t *) { detail::desk_go_to(shortcut_target()); }, LV_EVENT_CLICKED,
                        nullptr);
}

void build_focus(lv_obj_t *dock)
{
    s_focus = button(dock, Button::Flat, nullptr, nullptr);
    lv_obj_set_size(s_focus, ITEM_W, 80);
    lv_obj_set_style_radius(s_focus, 24, 0);
    s_focus_glyph = icon(s_focus, ic::timer, 34, false, Ink::Secondary);
    lv_obj_center(s_focus_glyph);
    s_focus_left = text(s_focus, Role::H3, "");
    lv_obj_center(s_focus_left);
    lv_obj_add_event_cb(s_focus, [](lv_event_t *e) { open_focus(lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);
    popover_source(s_focus);
}
}  // namespace

DockItems build_dock(lv_obj_t *chrome)
{
    lv_obj_t *dock = glass(chrome, Glass::Dock);
    lv_obj_set_style_radius(dock, radius::dock, 0);
    lv_obj_set_pos(dock, 14, 14);
    lv_obj_set_size(dock, size::dock, size::screen_h - 28);
    lv_obj_set_style_pad_all(dock, space::m, 0);
    lv_obj_set_flex_flow(dock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(dock, space::m, 0);

    build_height(dock);
    build_shortcut(dock);
    filler(dock);
    DockItems items;
    items.home = dock_item(dock, ic::home);
    filler(dock);
    items.calendar = dock_item(dock, ic::calendar_month);
    items.radar    = dock_item(dock, ic::flight);
    s_focus_rule = hairline(dock, false, 40);
    build_focus(dock);

    subscribe(Topic::Desk, detail::kNoView, paint_desk);
    follow_desk();
    subscribe(Topic::Focus, detail::kNoView, paint_focus);
    subscribe(Topic::Second, detail::kNoView, paint_focus);
    subscribe(Topic::Status, detail::kNoView, paint_focus);
    return items;
}

}  // namespace ui::kitfront
