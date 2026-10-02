#include "ui_internal.h"

#include "focus_model.h"
#include "focus_page.h"
#include "settings_model.h"
#include "status_model.h"
#include "topics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <utility>

// What every fullscreen view has over it: Stand and Sit at the left, the time
// and the way back at the right, there while the screen is being touched; and
// beside the time, the focus timer while it runs, there all the while. The
// pages have the same row over them, with the network, the phone and any
// update beside the time.
namespace ui::detail {
namespace {
constexpr std::int32_t  INSET     = 24;  // near the corners, leaving the middle to the view
constexpr std::int32_t  CHIP_STEP = theme::space::m;  // between the chips at the left, as Stand and Sit
constexpr std::int32_t  CHIP_MARK = 30;               // a mark's longer side in a chip, as the desk's
constexpr std::int32_t  CHIP_GAP  = 20;
constexpr std::int32_t  BADGE_H   = 44;
constexpr std::int32_t  BADGE_PAD = 16;
constexpr std::int32_t  BADGE_DOT = 10;
constexpr std::int32_t  BADGE_GAP = 10;               // between its dot and its time
constexpr lv_opa_t      PAUSED_OPA = LV_OPA_50;
constexpr std::int32_t  CELL_W     = 30;   // the battery drawn: its body,
constexpr std::int32_t  CELL_H     = 16;
constexpr std::int32_t  CELL_LINE  = 2;
constexpr std::int32_t  CELL_NUB_W = 3;    // and the nub at its end
constexpr std::int32_t  CELL_NUB_H = 6;
constexpr int           LOW_PERCENT = 20;
constexpr time_t        CLOCK_SET = 1'700'000'000;  // any earlier and the clock is not set yet

struct Clock {
    lv_obj_t *label = nullptr;
    lv_obj_t *chip  = nullptr;
    lv_obj_t *badge = nullptr;  // the focus timer, or null where it is the view
    lv_obj_t *dot   = nullptr;
    lv_obj_t *left  = nullptr;
    lv_obj_t *power = nullptr;  // fades with the buttons; what is in it shows only unplugged
    lv_obj_t *cell  = nullptr;
    lv_obj_t *fill  = nullptr;
};
std::deque<Clock> s_clocks;  // a deque, so what follows each keeps its place

// The time to the left of the chip, the battery left of it while unplugged,
// and the focus badge left of those.
void place(const Clock &clock)
{
    lv_obj_align_to(clock.label, clock.chip, LV_ALIGN_OUT_LEFT_MID, -CHIP_GAP, 0);
    lv_obj_align_to(clock.power, clock.label, LV_ALIGN_OUT_LEFT_MID, -CHIP_GAP, 0);
    if (clock.badge != nullptr) {
        lv_obj_t *beside = status_state().on_battery ? clock.power : clock.label;
        lv_obj_align_to(clock.badge, beside, LV_ALIGN_OUT_LEFT_MID, -CHIP_GAP, 0);
    }
}

// How full the pack is, while the panel runs on it: the cell filled as far,
// amber once low.
void show_power(const Clock &clock)
{
    const StatusState &status = status_state();
    lv_obj_set_hidden(clock.cell, !status.on_battery);
    if (!status.on_battery) {
        return;
    }
    const int percent = std::clamp(status.battery_percent, 0, 100);
    lv_obj_set_width(clock.fill, std::max<std::int32_t>(1, (CELL_W - 4 * CELL_LINE) * percent / 100));
    theme::set_bg_color(clock.fill, percent <= LOW_PERCENT ? theme::amber : theme::text);
}

void build_power(Clock &clock, lv_obj_t *root)
{
    clock.power = lv_obj_create(root);
    lv_obj_remove_style_all(clock.power);
    lv_obj_set_size(clock.power, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_clickable(clock.power, false);

    clock.cell = lv_obj_create(clock.power);
    lv_obj_remove_style_all(clock.cell);
    lv_obj_set_size(clock.cell, CELL_W + CELL_NUB_W, CELL_H);
    lv_obj_set_clickable(clock.cell, false);
    lv_obj_t *body = lv_obj_create(clock.cell);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, CELL_W, CELL_H);
    lv_obj_set_style_border_width(body, CELL_LINE, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_radius(body, CELL_LINE + 1, 0);
    lv_obj_t *nub = lv_obj_create(clock.cell);
    lv_obj_remove_style_all(nub);
    theme::style_panel(nub, theme::secondary, 1);
    lv_obj_set_size(nub, CELL_NUB_W, CELL_NUB_H);
    lv_obj_align(nub, LV_ALIGN_RIGHT_MID, 0, 0);
    clock.fill = lv_obj_create(clock.cell);
    lv_obj_remove_style_all(clock.fill);
    theme::style_panel(clock.fill, theme::text, 1);
    lv_obj_set_size(clock.fill, 1, CELL_H - 4 * CELL_LINE);
    lv_obj_set_pos(clock.fill, 2 * CELL_LINE, 2 * CELL_LINE);
}

void tell_time(const Clock &clock)
{
    const time_t now = std::time(nullptr);
    std::tm      local{};
    localtime_r(&now, &local);
    char text[8] = "";
    if (now >= CLOCK_SET) {
        std::strftime(text, sizeof(text), "%H:%M", &local);
    }
    if (std::strcmp(lv_label_get_text(clock.label), text) != 0) {
        theme::set_text(clock.label, text);
    }
}

void show_focus(const Clock &clock)
{
    const Focus &focus = focus_state();
    lv_obj_set_hidden(clock.badge, focus_idle(focus));
    if (focus_idle(focus)) {
        return;
    }
    char text[16];
    focus_clock_text(focus, text, sizeof(text));
    if (std::strcmp(lv_label_get_text(clock.left), text) != 0) {
        theme::set_text(clock.left, text);
    }
    theme::set_bg_color(clock.dot, focus_ink(focus_resting(focus)));
    lv_obj_set_style_opa(clock.badge, focus_paused(focus) ? PAUSED_OPA : static_cast<lv_opa_t>(LV_OPA_COVER), 0);
}

// The focus timer, as the tab shows it: its colour and how long is left, and a
// tap opens it.
void build_badge(Clock &clock, lv_obj_t *root, lv_event_cb_t on_click)
{
    clock.badge = lv_obj_create(root);
    theme::style_panel(clock.badge, theme::panel, theme::radius::pill);
    lv_obj_set_height(clock.badge, BADGE_H);
    lv_obj_set_style_pad_hor(clock.badge, BADGE_PAD, 0);
    lv_obj_set_style_pad_ver(clock.badge, 0, 0);
    lv_obj_set_flex_flow(clock.badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock.badge, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock.badge, BADGE_GAP, 0);
    lv_obj_set_scrollable(clock.badge, false);
    lv_obj_set_ext_click_area(clock.badge, theme::space::s);
    lv_obj_add_event_cb(clock.badge, on_click, LV_EVENT_CLICKED, nullptr);
    clock.dot = lv_obj_create(clock.badge);
    theme::style_panel(clock.dot, theme::primary, theme::radius::pill);
    lv_obj_set_size(clock.dot, BADGE_DOT, BADGE_DOT);
    lv_obj_set_clickable(clock.dot, false);
    clock.left = theme::make_label(clock.badge, "", theme::text, fonts::size_22());
    lv_obj_set_clickable(clock.left, false);
    // As wide as the widest it says, so neither it nor its dot moves as the
    // digits go by or it comes to Ready.
    std::int32_t widest = 0;
    for (const char *text : {"00:00", "Ready"}) {
        lv_point_t size{};
        lv_text_get_size(&size, text, fonts::size_22(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        widest = std::max(widest, size.x);
    }
    lv_obj_set_width(clock.left, widest);
    lv_obj_set_width(clock.badge, 2 * BADGE_PAD + BADGE_DOT + BADGE_GAP + widest);
    lv_obj_set_hidden(clock.badge, true);
}
}  // namespace

namespace {
constexpr std::int32_t STATUS_GAP       = 20;  // between what the top row holds
constexpr std::int32_t SLOT_W           = 40;  // each of the status's marks, centred in one as wide
constexpr std::int32_t SLOT_GAP         = 8;
constexpr std::int32_t TAP_MARGIN       = 12;  // round the badge and the status, to be hit easily
constexpr std::int32_t UPDATE_ICON_SIDE = 28;
constexpr std::int32_t UPDATE_BAR_H     = 3;
constexpr std::int32_t UPDATE_BAR_GAP   = 4;
constexpr std::int32_t PERCENT_ALL      = 100;

constexpr std::int32_t STATUS_PAD = 16;  // inside the status, which opens Setup
constexpr std::int32_t SETUP_DOT  = 10;  // an update waiting there

lv_obj_t *s_top_bar     = nullptr;
lv_obj_t *s_status      = nullptr;
lv_obj_t *s_setup_dot   = nullptr;
Clock    *s_top         = nullptr;
lv_obj_t *s_wifi_icon   = nullptr;
lv_obj_t *s_phone_icon  = nullptr;
lv_obj_t *s_slots[4]    = {};       // the battery, the phone, Wi-Fi and the time, from the page out
lv_obj_t *s_idle_mark   = nullptr;  // the timer drawn in the badge while it is idle
std::int32_t s_badge_w  = 0;        // the badge's width while the timer runs
lv_obj_t *s_update_box  = nullptr;  // while an update arrives, for whichever board
lv_obj_t *s_update_icon = nullptr;
lv_obj_t *s_update_bar  = nullptr;  // how far it is, under the icon

lv_obj_t *make_status_icon(lv_obj_t *parent, const lv_image_dsc_t *src)
{
    lv_obj_t *icon = lv_image_create(parent);
    lv_image_set_src(icon, src);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(theme::text), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_clickable(icon, false);
    return icon;
}

void build_update(lv_obj_t *bar)
{
    s_update_box = lv_obj_create(bar);
    lv_obj_remove_style_all(s_update_box);
    lv_obj_set_size(s_update_box, UPDATE_ICON_SIDE, UPDATE_ICON_SIDE + 2 * (UPDATE_BAR_GAP + UPDATE_BAR_H));
    lv_obj_set_clickable(s_update_box, false);
    s_update_icon = make_status_icon(s_update_box, &icons::update_panel_icon);
    lv_obj_center(s_update_icon);
    s_update_bar = lv_bar_create(s_update_box);
    lv_obj_set_size(s_update_bar, UPDATE_ICON_SIDE, UPDATE_BAR_H);
    lv_obj_align(s_update_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_update_bar, 0, PERCENT_ALL);
    theme::style_panel(s_update_bar, theme::panel_light, UPDATE_BAR_H / 2);
    theme::fill_accent(s_update_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_update_bar, UPDATE_BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_hidden(s_update_box, true);
}

void show_top_focus();

void paint_status()
{
    const StatusState &status = status_state();
    static int s_phone_shown = -1;
    if (std::exchange(s_phone_shown, status.present ? 1 : 0) != (status.present ? 1 : 0)) {
        lv_image_set_src(s_phone_icon, status.present ? &icons::phone_icon : &icons::phone_off_icon);
    }
    static int s_wifi_shown = -1;
    if (std::exchange(s_wifi_shown, status.wifi ? 1 : 0) != (status.wifi ? 1 : 0)) {
        lv_image_set_src(s_wifi_icon, status.wifi ? &icons::wifi_icon : &icons::wifi_off_icon);
    }
    show_power(*s_top);
    lv_obj_set_hidden(s_slots[0], !status.on_battery);
    show_top_focus();
}

// The focus timer, there idle too with the round it would start, as the way
// to it; away with the owner's pages while the phone is.
void show_top_focus()
{
    show_focus(*s_top);
    const bool away = s_presence_gate && !status_state().present;
    const bool idle = focus_idle(focus_state());
    // Idle, only the timer drawn, as round as the row is tall.
    lv_obj_set_hidden(s_top->badge, away);
    lv_obj_set_hidden(s_idle_mark, !idle);
    lv_obj_set_hidden(s_top->dot, idle);
    lv_obj_set_hidden(s_top->left, idle);
    lv_obj_set_width(s_top->badge, idle ? TOP_H : s_badge_w);
    lv_obj_set_style_pad_hor(s_top->badge, idle ? 0 : BADGE_PAD, 0);
}

lv_obj_t *make_slot(lv_obj_t *parent, std::int32_t w)
{
    lv_obj_t *slot = lv_obj_create(parent);
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, w, TOP_H);
    lv_obj_set_clickable(slot, false);
    lv_obj_set_scrollable(slot, false);
    return slot;
}

// The status against the dock, the time at its edge, and the badge beside it.
void arrange_top()
{
    const bool right = layout().rail_right;
    lv_obj_set_flex_align(s_top_bar, right ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *const row[] = {s_top->badge, s_update_box, s_status};
    for (int i = 0; i < 3; ++i) {
        lv_obj_move_to_index(row[right ? i : 2 - i], i);
    }
    for (int i = 0; i < 4; ++i) {
        lv_obj_move_to_index(s_slots[right ? i : 3 - i], i);
    }
    lv_obj_set_style_text_align(s_top->label, right ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_setup_dot, right ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, right ? STATUS_PAD / 2 : -STATUS_PAD / 2, 0);
}
}  // namespace

void create_top_bar(lv_obj_t *parent)
{
    s_top_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_top_bar);
    lv_obj_set_height(s_top_bar, TOP_H);
    lv_obj_set_flex_flow(s_top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_top_bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_top_bar, STATUS_GAP, 0);
    lv_obj_set_scrollable(s_top_bar, false);
    lv_obj_set_clickable(s_top_bar, false);

    s_clocks.emplace_back();
    s_top = &s_clocks.back();
    build_badge(*s_top, s_top_bar, [](lv_event_t *) { toggle_focus_popout(s_top->badge, s_top_bar); });
    lv_obj_set_style_bg_color(s_top->badge, lv_color_hex(theme::panel_light), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_top->badge, lv_color_hex(theme::panel_light), LV_STATE_PRESSED);
    lv_obj_set_height(s_top->badge, TOP_H);
    lv_obj_set_flex_align(s_top->badge, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(s_top->badge, TAP_MARGIN);
    s_badge_w   = lv_obj_get_style_width(s_top->badge, LV_PART_MAIN);
    s_idle_mark = make_status_icon(s_top->badge, &icons::timer_icon);
    lv_obj_set_style_image_recolor(s_idle_mark, lv_color_hex(theme::secondary), 0);
    build_update(s_top_bar);

    // The battery, the phone, Wi-Fi and the time are where Setup opens.
    s_status = lv_button_create(s_top_bar);
    lv_obj_remove_style_all(s_status);
    lv_obj_set_size(s_status, LV_SIZE_CONTENT, TOP_H);
    lv_obj_set_flex_flow(s_status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(s_status, STATUS_PAD, 0);
    lv_obj_set_style_pad_column(s_status, SLOT_GAP, 0);
    lv_obj_set_ext_click_area(s_status, TAP_MARGIN);
    lv_obj_set_style_radius(s_status, theme::radius::pill, 0);
    for (const lv_state_t state : {LV_STATE_PRESSED, LV_STATE_CHECKED}) {
        lv_obj_set_style_bg_color(s_status, lv_color_hex(theme::panel_light), state);
        lv_obj_set_style_bg_opa(s_status, LV_OPA_COVER, state);
    }
    lv_obj_add_event_cb(s_status, [](lv_event_t *) { toggle_setup(); }, LV_EVENT_CLICKED, nullptr);
    for (lv_obj_t *&slot : s_slots) {
        slot = make_slot(s_status, SLOT_W);
    }
    build_power(*s_top, s_slots[0]);
    lv_obj_center(s_top->power);
    s_phone_icon = make_status_icon(s_slots[1], &icons::phone_off_icon);
    s_wifi_icon  = make_status_icon(s_slots[2], &icons::wifi_off_icon);
    lv_obj_center(s_phone_icon);
    lv_obj_center(s_wifi_icon);
    s_top->label = theme::make_label(s_slots[3], "00:00", theme::text, fonts::size_28());
    std::int32_t widest = 0;  // so the status keeps its width as the minutes go by
    for (char d = '0'; d <= '9'; ++d) {
        const char text[] = {d, d, ':', d, d, '\0'};
        lv_point_t size{};
        lv_text_get_size(&size, text, fonts::size_28(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        widest = std::max(widest, size.x);
    }
    lv_obj_set_width(s_slots[3], widest);
    theme::center_ink(s_top->label);  // level with the marks, by the digits rather than the line
    lv_obj_set_width(s_top->label, widest);
    lv_obj_set_x(s_top->label, 0);
    lv_obj_set_align(s_top->label, LV_ALIGN_LEFT_MID);
    s_setup_dot = lv_obj_create(s_status);
    theme::style_panel(s_setup_dot, theme::panel, SETUP_DOT / 2);
    theme::fill_accent(s_setup_dot);
    lv_obj_set_size(s_setup_dot, SETUP_DOT, SETUP_DOT);
    lv_obj_set_clickable(s_setup_dot, false);
    lv_obj_set_ignore_layout(s_setup_dot, true);
    lv_obj_align(s_setup_dot, LV_ALIGN_TOP_RIGHT, STATUS_PAD / 2, 0);
    lv_obj_set_hidden(s_setup_dot, true);
    place_top_bar();

    subscribe(Topic::Status, kNoView, paint_status);
    subscribe(Topic::Second, kNoView, [] {
        tell_time(*s_top);
        show_top_focus();
    });
    subscribe(Topic::Focus, kNoView, show_top_focus);
    subscribe(Topic::Page, kNoView, [] {
        show_top_focus();
        lv_obj_set_state(s_status, LV_STATE_CHECKED, s_page == SETUP_PAGE);
    });
    subscribe(Topic::Update, kNoView, [] {
        if (settings_state().update_known) {
            paint_update_icon(settings_state().update);
        }
    });
}

// Along the top of the pages, ending level with their right edge.
void place_top_bar()
{
    const Layout l = layout();
    lv_obj_set_pos(s_top_bar, l.content_x, TOP_Y);
    lv_obj_set_width(s_top_bar, l.content_w);
    arrange_top();
}

void paint_setup_dot(bool ready)
{
    if (s_setup_dot != nullptr) {
        lv_obj_set_hidden(s_setup_dot, !ready);
    }
}

const lv_image_dsc_t *update_icon(const UpdateState &state)
{
    if (state.busy == UpdateTarget::Panel) {
        return &icons::update_panel_icon;
    }
    return state.phase == UpdatePhase::Installing ? &icons::install_companion_icon
                                                  : &icons::update_companion_icon;
}

void paint_update_icon(const UpdateState &state)
{
    if (s_update_box == nullptr) {
        return;
    }
    const bool busy = state.busy != UpdateTarget::None;
    lv_obj_set_hidden(s_update_box, !busy);
    if (busy) {
        // Accent for one that installs, and restarts, as soon as it is in.
        lv_obj_set_style_image_recolor(
            s_update_icon, lv_color_hex(state.immediate ? theme::primary : theme::text), 0);
        lv_image_set_src(s_update_icon, update_icon(state));
        lv_bar_set_value(s_update_bar, state.percent, LV_ANIM_OFF);
    }
}

ViewClock add_view_clock(ViewId view, lv_obj_t *root, lv_obj_t *chip, bool focus_badge)
{
    s_clocks.push_back({.label = theme::make_label(root, "", theme::text, fonts::size_28()), .chip = chip});
    Clock &clock = s_clocks.back();
    lv_obj_set_clickable(clock.label, false);
    fade_when_idle(view, clock.label);
    build_power(clock, root);
    fade_when_idle(view, clock.power);
    if (focus_badge) {
        build_badge(clock, root, [](lv_event_t *) { open_focus_full(); });
    }
    subscribe(Topic::Status, view, [&clock] {
        show_power(clock);
        place(clock);
    });
    subscribe(Topic::Second, view, [&clock] {
        tell_time(clock);
        if (clock.badge != nullptr) {
            show_focus(clock);
        }
        place(clock);
    });
    if (focus_badge) {
        subscribe(Topic::Focus, view, [&clock] {
            show_focus(clock);
            place(clock);
        });
    }
    return {clock.label, clock.badge, clock.power};
}

Chrome add_fullscreen_chrome(ViewId view, lv_obj_t *root, lv_event_cb_t on_close, bool focus_badge)
{
    lv_obj_t *close = theme::make_chip(root, "");
    theme::make_mark(close, &icons::collapse_icon);
    lv_obj_set_ext_click_area(close, theme::space::s);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, nullptr);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -INSET, INSET);
    lv_obj_t *desk = add_desk_shortcuts(root, INSET, INSET, theme::panel);
    for (lv_obj_t *control : {close, desk}) {
        fade_when_idle(view, control);
    }
    const ViewClock clock = add_view_clock(view, root, close, focus_badge);
    const std::int32_t after = desk != nullptr ? INSET + lv_obj_get_style_width(desk, LV_PART_MAIN) + CHIP_STEP : INSET;
    return {view, root, close, desk, clock.badge, after};
}

lv_obj_t *add_chrome_chip(Chrome &chrome, const lv_image_dsc_t *icon, lv_event_cb_t on_click)
{
    lv_obj_t *chip = theme::make_chip(chrome.root, "");
    lv_obj_set_pos(chip, chrome.next_x, INSET);
    lv_obj_set_ext_click_area(chip, CHIP_STEP / 2);
    lv_obj_add_event_cb(chip, on_click, LV_EVENT_CLICKED, nullptr);
    theme::fill_accent(chip, LV_STATE_CHECKED);
    lv_obj_t *mark = theme::make_mark(chip, icon);
    lv_image_set_scale(mark, LV_SCALE_NONE * CHIP_MARK / std::max<std::int32_t>(icon->header.w, icon->header.h));
    lv_obj_set_style_image_recolor(mark, lv_color_hex(theme::text), LV_STATE_CHECKED);
    lv_obj_set_style_image_opa(mark, LV_OPA_COVER, LV_STATE_CHECKED);
    fade_when_idle(chrome.view, chip);
    chrome.next_x += theme::chip::size + CHIP_STEP;
    return chip;
}

void light_chrome_chip(lv_obj_t *chip, bool on)
{
    lv_obj_set_state(chip, LV_STATE_CHECKED, on);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(chip); ++i) {
        lv_obj_set_state(lv_obj_get_child(chip, static_cast<std::int32_t>(i)), LV_STATE_CHECKED, on);
    }
}
}  // namespace ui::detail
