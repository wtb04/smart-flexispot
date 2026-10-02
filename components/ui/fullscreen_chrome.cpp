#include "ui_internal.h"

#include "focus_model.h"
#include "focus_page.h"
#include "topics.h"

#include <algorithm>
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
constexpr time_t        CLOCK_SET = 1'700'000'000;  // any earlier and the clock is not set yet

struct Clock {
    lv_obj_t *label;
    lv_obj_t *chip;
    lv_obj_t *badge;  // the focus timer, or null where it is the view
    lv_obj_t *dot;
    lv_obj_t *left;
};
std::deque<Clock> s_clocks;  // a deque, so what follows each keeps its place

// The time to the left of the chip, and the badge to the left of the time.
void place(const Clock &clock)
{
    lv_obj_align_to(clock.label, clock.chip, LV_ALIGN_OUT_LEFT_MID, -CHIP_GAP, 0);
    if (clock.badge != nullptr) {
        lv_obj_align_to(clock.badge, clock.label, LV_ALIGN_OUT_LEFT_MID, -CHIP_GAP, 0);
    }
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
    lv_obj_set_style_opa(clock.badge, focus_paused(focus) ? PAUSED_OPA : LV_OPA_COVER, 0);
}

// The focus timer, as the tab shows it: its colour and how long is left, and a
// tap opens it.
void build_badge(Clock &clock, lv_obj_t *root)
{
    clock.badge = lv_obj_create(root);
    theme::style_panel(clock.badge, theme::panel, theme::radius::pill);
    lv_obj_set_size(clock.badge, LV_SIZE_CONTENT, BADGE_H);
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
    lv_obj_set_hidden(clock.badge, true);
}
}  // namespace

ViewClock add_view_clock(ViewId view, lv_obj_t *root, lv_obj_t *chip, bool focus_badge)
{
    s_clocks.push_back({theme::make_label(root, "", theme::text, fonts::size_28()), chip});
    Clock &clock = s_clocks.back();
    lv_obj_set_clickable(clock.label, false);
    fade_when_idle(view, clock.label);
    if (focus_badge) {
        build_badge(clock, root);
    }
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
    return {clock.label, clock.badge};
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
