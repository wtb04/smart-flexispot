#include "ui_internal.h"

#include "focus_model.h"
#include "focus_page.h"
#include "status_model.h"
#include "topics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>

// What every fullscreen view has over it: Stand and Sit at the left, the time
// and the way back at the right, there while the screen is being touched; and
// beside the time, the focus timer while it runs, there all the while.
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
constexpr std::int32_t  CELL_GAP   = 8;    // between it and its percent
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
    lv_obj_t *percent = nullptr;
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

// How full the pack is, while the panel runs on it.
void show_power(const Clock &clock)
{
    const StatusState &status = status_state();
    lv_obj_set_hidden(clock.cell, !status.on_battery);
    lv_obj_set_hidden(clock.percent, !status.on_battery);
    if (!status.on_battery) {
        return;
    }
    const int      percent = std::clamp(status.battery_percent, 0, 100);
    const std::uint32_t ink = percent <= LOW_PERCENT ? theme::amber : theme::text;
    lv_obj_set_width(clock.fill, std::max<std::int32_t>(1, (CELL_W - 4 * CELL_LINE) * percent / 100));
    theme::set_bg_color(clock.fill, ink);
    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(clock.percent, text);
    theme::set_text_color(clock.percent, ink);
}

void build_power(Clock &clock, lv_obj_t *root)
{
    clock.power = lv_obj_create(root);
    lv_obj_remove_style_all(clock.power);
    lv_obj_set_size(clock.power, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clock.power, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock.power, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock.power, CELL_GAP, 0);
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

    clock.percent = theme::make_label(clock.power, "", theme::text, fonts::size_22());
    lv_point_t widest{};
    lv_text_get_size(&widest, "100%", fonts::size_22(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(clock.percent, widest.x);  // one width, whatever its digits say
    lv_obj_set_style_text_align(clock.percent, LV_TEXT_ALIGN_RIGHT, 0);
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
void build_badge(Clock &clock, lv_obj_t *root)
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
    lv_obj_add_event_cb(clock.badge, [](lv_event_t *) { open_focus_full(); }, LV_EVENT_CLICKED, nullptr);
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

ViewClock add_view_clock(ViewId view, lv_obj_t *root, lv_obj_t *chip, bool focus_badge)
{
    s_clocks.push_back({.label = theme::make_label(root, "", theme::text, fonts::size_28()), .chip = chip});
    Clock &clock = s_clocks.back();
    lv_obj_set_clickable(clock.label, false);
    fade_when_idle(view, clock.label);
    build_power(clock, root);
    fade_when_idle(view, clock.power);
    if (focus_badge) {
        build_badge(clock, root);
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
