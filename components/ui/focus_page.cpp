#include "focus_page.h"

#include "ui_internal.h"

#include "esp_timer.h"
#include "fonts/units_font.h"

#include <algorithm>
#include <cstdio>

namespace ui {
namespace {
namespace space = theme::space;

constexpr std::int32_t SIDE_W    = 340;
constexpr std::int32_t RING_W    = 18;
constexpr std::int32_t DOT       = 14;
constexpr std::int32_t COLON     = 10;
constexpr std::int32_t PRIMARY_H = 128;
constexpr std::int32_t BUTTON_H  = 76;
constexpr int          ROUNDS_MAX = 8;

// As last told; until then, the plan as it stands at boot.
Focus s_focus{FocusPhase::Idle, 0, 4, false, 0, 0, 0, 30, 5, 20};

lv_obj_t *s_ring      = nullptr;
lv_obj_t *s_phase     = nullptr;
lv_obj_t *s_minutes   = nullptr;
lv_obj_t *s_seconds   = nullptr;
lv_obj_t *s_clock     = nullptr;  // the two numbers and the colon between them
lv_obj_t *s_round     = nullptr;
lv_obj_t *s_dot[ROUNDS_MAX];
lv_obj_t *s_primary   = nullptr;
lv_obj_t *s_skip      = nullptr;
lv_obj_t *s_reset     = nullptr;
lv_obj_t *s_next      = nullptr;
lv_obj_t *s_next_what = nullptr;
lv_obj_t *s_plan      = nullptr;
int       s_shown_s   = -1;

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

std::int32_t left_ms()
{
    if (s_focus.phase == FocusPhase::Idle) {
        return s_focus.work_min * 60 * 1000;
    }
    if (!s_focus.running) {
        return s_focus.left_ms;
    }
    return static_cast<std::int32_t>(std::max<std::int64_t>(0, s_focus.ends_at_ms - now_ms()));
}

void show_time()
{
    const std::int32_t left    = left_ms();
    const int          seconds = static_cast<int>((left + 999) / 1000);
    if (seconds == s_shown_s) {
        return;
    }
    s_shown_s = seconds;
    char text[8];
    std::snprintf(text, sizeof(text), "%02d", seconds / 60);
    theme::set_text(s_minutes, text);
    std::snprintf(text, sizeof(text), "%02d", seconds % 60);
    theme::set_text(s_seconds, text);
    const std::int32_t length = s_focus.length_ms > 0 ? s_focus.length_ms : left;
    lv_arc_set_value(s_ring, length > 0 ? static_cast<std::int32_t>(
                                              static_cast<std::int64_t>(left) * 1000 / length)
                                        : 1000);
}

const char *phase_name(FocusPhase phase)
{
    switch (phase) {
        case FocusPhase::Work:      return "FOCUS";
        case FocusPhase::Break:     return "BREAK";
        case FocusPhase::LongBreak: return "LONG BREAK";
        case FocusPhase::Idle:      break;
    }
    return "READY";
}

// Faded rather than disabled: LVGL greys a disabled button lighter than an
// enabled one, which reads the wrong way round.
void enable(lv_obj_t *button, bool on)
{
    lv_obj_set_clickable(button, on);
    lv_obj_set_style_opa(button, on ? LV_OPA_COVER : LV_OPA_40, 0);
}

void timer_tick(lv_timer_t *)
{
    if (detail::s_page == detail::FOCUS_PAGE && s_focus.running) {
        show_time();
    }
}

void clicked(lv_event_t *e)
{
    const auto action = static_cast<FocusAction>(
        reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (detail::s_handlers.focus != nullptr) {
        detail::s_handlers.focus(action);
    }
}

lv_obj_t *button(lv_obj_t *parent, const char *text, FocusAction action, bool primary)
{
    lv_obj_t *btn = theme::make_button(parent, text, theme::panel,
                                       primary ? theme::type_title() : theme::type_body());
    if (primary) {
        theme::fill_accent(btn);
    }
    lv_obj_add_event_cb(btn, clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(action)));
    return btn;
}

}  // namespace

void show_focus(const Focus &focus)
{
    s_focus   = focus;
    s_shown_s = -1;
    if (s_ring == nullptr) {
        return;
    }
    const bool idle    = focus.phase == FocusPhase::Idle;
    const bool resting = focus.phase == FocusPhase::Break || focus.phase == FocusPhase::LongBreak;

    char text[64];
    if (!idle && !focus.running) {
        std::snprintf(text, sizeof(text), "%s, PAUSED", phase_name(focus.phase));
    } else {
        std::snprintf(text, sizeof(text), "%s", phase_name(focus.phase));
    }
    theme::set_text(s_phase, text);
    theme::arc_accent_or(s_ring, !resting, theme::green, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_clock, !idle && !focus.running ? LV_OPA_60 : LV_OPA_COVER, 0);

    switch (focus.phase) {
        case FocusPhase::Idle:
            std::snprintf(text, sizeof(text), "%d rounds of %d min", focus.rounds, focus.work_min);
            break;
        case FocusPhase::LongBreak:
            std::snprintf(text, sizeof(text), "All %d rounds done", focus.rounds);
            break;
        default:
            std::snprintf(text, sizeof(text), "Round %d of %d", focus.round, focus.rounds);
            break;
    }
    theme::set_text(s_round, text);

    // A round is done once its focus has run; the one under way is outlined.
    const int done = focus.phase == FocusPhase::Work ? focus.round - 1 : idle ? 0 : focus.round;
    for (int i = 0; i < ROUNDS_MAX; ++i) {
        lv_obj_t *dot = s_dot[i];
        lv_obj_set_hidden(dot, i >= focus.rounds);
        const bool filled  = i < done;
        const bool current = focus.phase == FocusPhase::Work && i == focus.round - 1;
        theme::fill_accent_or(dot, filled, theme::panel);
        lv_obj_set_style_border_color(dot, lv_color_hex(theme::primary), 0);
        lv_obj_set_style_border_width(dot, current ? 2 : 0, 0);
    }

    theme::set_text(lv_obj_get_child(s_primary, 0), idle ? "Start" : focus.running ? "Pause" : "Resume");
    enable(s_skip, !idle);
    enable(s_reset, !idle);

    switch (focus.phase) {
        case FocusPhase::Idle:
            std::snprintf(text, sizeof(text), "Focus, %d min", focus.work_min);
            break;
        case FocusPhase::Work:
            if (focus.round >= focus.rounds) {
                std::snprintf(text, sizeof(text), "Long break, %d min", focus.long_break_min);
            } else {
                std::snprintf(text, sizeof(text), "Break, %d min", focus.break_min);
            }
            break;
        case FocusPhase::Break:
            std::snprintf(text, sizeof(text), "Round %d, %d min", focus.round + 1, focus.work_min);
            break;
        case FocusPhase::LongBreak:
            std::snprintf(text, sizeof(text), "Nothing, the set is done");
            break;
    }
    theme::set_text(s_next_what, text);
    std::snprintf(text, sizeof(text), "%d min focus, %d min breaks, %d min after round %d",
                  focus.work_min, focus.break_min, focus.long_break_min, focus.rounds);
    theme::set_text(s_plan, text);

    show_time();
}

void build_focus_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    const std::int32_t card_w = width - SIDE_W - space::m;

    lv_obj_t *clock_card = theme::make_card(page);
    lv_obj_set_pos(clock_card, 0, 0);
    lv_obj_set_size(clock_card, card_w, height);
    lv_obj_set_scrollable(clock_card, false);

    const std::int32_t ring = std::min(card_w, height) - 2 * space::l;
    s_ring                  = lv_arc_create(clock_card);
    lv_obj_set_size(s_ring, ring, ring);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 1000);
    lv_obj_remove_style(s_ring, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(theme::panel), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_ring, true, LV_PART_INDICATOR);
    theme::arc_accent(s_ring, LV_PART_INDICATOR);

    lv_obj_t *middle = lv_obj_create(clock_card);
    theme::style_panel(middle, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(middle, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(middle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(middle, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(middle, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(middle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(middle, space::s, 0);
    lv_obj_center(middle);

    s_phase = theme::make_eyebrow(middle, "READY");

    // The big digits have no colon, so it is drawn: two dots between them.
    s_clock = lv_obj_create(middle);
    theme::style_panel(s_clock, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(s_clock, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_clock, space::s, 0);
    s_minutes = theme::make_label(s_clock, "30", theme::text, fonts::temp_64());
    lv_obj_t *colon = lv_obj_create(s_clock);
    theme::style_panel(colon, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(colon, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(colon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(colon, COLON, 3 * COLON + space::m);
    lv_obj_set_style_margin_top(colon, space::m, 0);  // on the digits' middle, not their line's
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *dot = lv_obj_create(colon);
        theme::style_panel(dot, theme::text, LV_RADIUS_CIRCLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, COLON, COLON);
        lv_obj_align(dot, i == 0 ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    s_seconds = theme::make_label(s_clock, "00", theme::text, fonts::temp_64());

    s_round = theme::make_label(middle, "", theme::secondary, theme::type_body());

    lv_obj_t *dots = lv_obj_create(middle);
    theme::style_panel(dots, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, DOT);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, space::s, 0);
    for (lv_obj_t *&dot : s_dot) {
        dot = lv_obj_create(dots);
        theme::style_panel(dot, theme::panel, LV_RADIUS_CIRCLE);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, DOT, DOT);
    }

    // Controls, then what comes next.
    const std::int32_t side_x = card_w + space::m;
    lv_obj_t *controls = theme::make_card(page);
    const std::int32_t controls_h = PRIMARY_H + BUTTON_H + space::s + 2 * space::l;
    lv_obj_set_pos(controls, side_x, 0);
    lv_obj_set_size(controls, SIDE_W, controls_h);
    lv_obj_set_scrollable(controls, false);
    const std::int32_t inner_w = SIDE_W - 2 * space::l;

    s_primary = button(controls, "Start", FocusAction::Toggle, true);
    lv_obj_set_pos(s_primary, 0, 0);
    lv_obj_set_size(s_primary, inner_w, PRIMARY_H);
    const std::int32_t half = (inner_w - space::s) / 2;
    s_skip = button(controls, "Skip", FocusAction::Skip, false);
    lv_obj_set_pos(s_skip, 0, PRIMARY_H + space::s);
    lv_obj_set_size(s_skip, half, BUTTON_H);
    s_reset = button(controls, "Reset", FocusAction::Reset, false);
    lv_obj_set_pos(s_reset, half + space::s, PRIMARY_H + space::s);
    lv_obj_set_size(s_reset, half, BUTTON_H);

    const std::int32_t next_y = controls_h + space::m;
    s_next = theme::make_card(page);
    lv_obj_set_pos(s_next, side_x, next_y);
    lv_obj_set_size(s_next, SIDE_W, height - next_y);
    lv_obj_set_scrollable(s_next, false);
    lv_obj_set_flex_flow(s_next, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_next, space::s, 0);
    theme::make_eyebrow(s_next, "UP NEXT");
    s_next_what = theme::make_label(s_next, "", theme::text, theme::type_value());
    lv_obj_set_width(s_next_what, inner_w);
    lv_obj_t *gap = lv_obj_create(s_next);
    theme::style_panel(gap, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(gap, LV_OPA_TRANSP, 0);
    lv_obj_set_size(gap, 1, 1);
    lv_obj_set_flex_grow(gap, 1);
    s_plan = theme::make_label(s_next, "", theme::secondary, theme::type_label());
    lv_obj_set_width(s_plan, inner_w);
    lv_label_set_long_mode(s_plan, LV_LABEL_LONG_MODE_WRAP);

    lv_timer_create(timer_tick, 200, nullptr);
    show_focus(s_focus);
}

}  // namespace ui
