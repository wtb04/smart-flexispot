#include "ui_internal.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <vector>

// What every fullscreen view has over it: Stand and Sit at the left, the time
// and the way back at the right, there while the screen is being touched.
namespace ui::detail {
namespace {
constexpr std::int32_t  INSET     = 40;
constexpr std::int32_t  CHIP_STEP = theme::space::m;  // between the chips at the left, as Stand and Sit
constexpr std::int32_t  CHIP_MARK = 30;               // a mark's longer side in a chip, as the desk's
constexpr std::int32_t  CHIP_GAP  = 20;
constexpr std::uint32_t TICK_MS   = 1000;
constexpr time_t        CLOCK_SET = 1'700'000'000;  // any earlier and the clock is not set yet

struct Clock {
    lv_obj_t *label;
    lv_obj_t *chip;
    lv_area_t beside;  // the chip it was put beside, where it was then
};
std::vector<Clock> s_clocks;

void tell_time(lv_timer_t *)
{
    update_view_clocks();
}
}  // namespace

void update_view_clocks()
{
    const time_t now = std::time(nullptr);
    std::tm      local{};
    localtime_r(&now, &local);
    char text[8] = "";
    if (now >= CLOCK_SET) {
        std::strftime(text, sizeof(text), "%H:%M", &local);
    }
    for (Clock &clock : s_clocks) {
        const bool changed = std::strcmp(lv_label_get_text(clock.label), text) != 0;
        if (changed) {
            theme::set_text(clock.label, text);
        }
        lv_area_t chip{};
        lv_obj_update_layout(clock.chip);
        lv_obj_get_coords(clock.chip, &chip);
        if (changed || std::memcmp(&chip, &clock.beside, sizeof(chip)) != 0) {
            clock.beside = chip;
            lv_obj_align_to(clock.label, clock.chip, LV_ALIGN_OUT_LEFT_MID, -CHIP_GAP, 0);
        }
    }
}

lv_obj_t *add_view_clock(ViewId view, lv_obj_t *root, lv_obj_t *chip)
{
    lv_obj_t *label = theme::make_label(root, "", theme::text, fonts::size_28());
    lv_obj_set_clickable(label, false);
    if (s_clocks.empty()) {
        lv_timer_create(tell_time, TICK_MS, nullptr);
    }
    s_clocks.push_back({label, chip, {}});
    fade_when_idle(view, label);
    update_view_clocks();
    return label;
}

Chrome add_fullscreen_chrome(ViewId view, lv_obj_t *root, lv_event_cb_t on_close)
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
    add_view_clock(view, root, close);
    const std::int32_t after = desk != nullptr ? INSET + lv_obj_get_style_width(desk, LV_PART_MAIN) + CHIP_STEP : INSET;
    return {view, root, close, desk, after};
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
