#include "focus_page.h"

#include "ui_internal.h"

#include "esp_timer.h"
#include "fonts/units_font.h"

#include <algorithm>
#include <ctime>
#include <cstdio>

// Laid out as the home page is, so the two read as one panel: a row of pills
// over a dial card with its screws and chips, and two cards beside it.
namespace ui {
namespace {
using detail::BUTTON_GAP;

constexpr std::int32_t PILL_H     = 60;
constexpr std::int32_t PILL_PAD   = 22;
constexpr std::int32_t DOT        = 14;
constexpr std::int32_t DIAL_W     = 480;
constexpr std::int32_t DIAL_INSET = 100;
constexpr std::int32_t CHIP       = theme::chip::size;
constexpr std::int32_t CHIP_GAP   = 10;
constexpr std::int32_t COLON      = 9;
constexpr std::int32_t ROUND_DOT  = 28;
constexpr int          ROUNDS_MAX = 8;
constexpr int          PILLS      = 3;  // focus, break, long break

// As last told; until then, the plan as it stands at boot.
Focus s_focus{FocusPhase::Idle, 0, 4, false, 0, 0, 0, 25, 5, 20};

struct PlanPill {
    lv_obj_t *dot   = nullptr;
    lv_obj_t *value = nullptr;
};
PlanPill  s_pill[PILLS];
lv_obj_t *s_dial      = nullptr;
lv_obj_t *s_phase     = nullptr;
lv_obj_t *s_clock     = nullptr;  // the two numbers and the colon between them
lv_obj_t *s_minutes   = nullptr;
lv_obj_t *s_seconds   = nullptr;
lv_obj_t *s_ends_name = nullptr;
lv_obj_t *s_ends      = nullptr;
lv_obj_t *s_go        = nullptr;
lv_obj_t *s_skip      = nullptr;
lv_obj_t *s_reset     = nullptr;
lv_obj_t *s_next      = nullptr;
lv_obj_t *s_next_long = nullptr;
lv_obj_t *s_round     = nullptr;
lv_obj_t *s_round_dot[ROUNDS_MAX];
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

// The ring fills as the part runs, its knob going round like a clock's hand.
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
    const std::int32_t length = s_focus.length_ms;
    lv_arc_set_value(s_dial, length > 0 ? static_cast<std::int32_t>(
                                              static_cast<std::int64_t>(length - left) * 1000 / length)
                                        : 0);
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
    const auto action =
        static_cast<FocusAction>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (detail::s_handlers.focus != nullptr) {
        detail::s_handlers.focus(action);
    }
}

void on_click(lv_obj_t *obj, FocusAction action)
{
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(action)));
}

lv_obj_t *clear_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    theme::style_panel(box, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(box, false);
    return box;
}

void build_pills(lv_obj_t *parent, std::int32_t w)
{
    static const char *const NAMES[PILLS] = {"FOCUS", "BREAK", "LONG BREAK"};
    const std::int32_t       pill_w       = (w - (PILLS - 1) * BUTTON_GAP) / PILLS;
    for (int i = 0; i < PILLS; ++i) {
        lv_obj_t *pill = lv_obj_create(parent);
        lv_obj_set_pos(pill, i * (pill_w + BUTTON_GAP), 0);
        lv_obj_set_size(pill, pill_w, PILL_H);
        theme::style_panel(pill, theme::panel_light, PILL_H / 2);
        lv_obj_set_clickable(pill, false);
        lv_obj_set_style_pad_hor(pill, PILL_PAD, 0);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(pill, 10, 0);

        s_pill[i].dot = lv_obj_create(pill);
        lv_obj_set_size(s_pill[i].dot, DOT, DOT);
        theme::style_panel(s_pill[i].dot, theme::secondary, DOT / 2);
        lv_obj_set_clickable(s_pill[i].dot, false);

        lv_obj_t *column = clear_box(pill);
        lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(column, -1, 0);
        theme::make_label(column, NAMES[i], theme::secondary, fonts::size_16());
        s_pill[i].value = theme::make_label(column, "", theme::text, fonts::size_22());
    }
}

void build_dial(lv_obj_t *parent, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, w, h);
    theme::style_panel(card, theme::panel_light, theme::radius::card);
    lv_obj_set_style_pad_all(card, detail::PANEL_PAD, 0);
    lv_obj_set_scrollable(card, false);

    const std::int32_t inner   = w - 2 * detail::PANEL_PAD;
    const std::int32_t inner_h = h - 2 * detail::PANEL_PAD;
    lv_obj_set_pos(theme::make_screw(card, CHIP), 0, 0);
    lv_obj_set_pos(theme::make_screw(card, CHIP), 0, inner_h - CHIP);
    lv_obj_set_pos(theme::make_screw(card, CHIP), inner - CHIP, inner_h - CHIP);

    s_skip = theme::make_chip(card, LV_SYMBOL_NEXT);
    lv_obj_set_pos(s_skip, inner - CHIP, 0);
    on_click(s_skip, FocusAction::Skip);
    s_reset = theme::make_chip(card, LV_SYMBOL_REFRESH);
    lv_obj_set_pos(s_reset, inner - 2 * CHIP - CHIP_GAP, 0);
    on_click(s_reset, FocusAction::Reset);

    const std::int32_t ring   = std::min(w - DIAL_INSET, inner_h);
    const std::int32_t ring_y = (inner_h - ring) / 2;
    s_dial                    = lv_arc_create(card);
    lv_obj_set_size(s_dial, ring, ring);
    lv_obj_align(s_dial, LV_ALIGN_TOP_MID, 0, ring_y);
    lv_arc_set_bg_angles(s_dial, 135, 45);
    lv_arc_set_rotation(s_dial, 0);
    lv_arc_set_range(s_dial, 0, 1000);
    lv_arc_set_value(s_dial, 0);
    lv_obj_remove_flag(s_dial, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_dial, 24, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_dial, lv_color_hex(theme::panel), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_dial, 24, LV_PART_INDICATOR);
    theme::arc_accent(s_dial, LV_PART_INDICATOR);
    theme::fill_accent(s_dial, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_dial, 10, LV_PART_KNOB);

    const std::int32_t centre = ring_y + ring / 2;
    s_phase = theme::make_label(card, "READY", theme::secondary, fonts::size_16());
    lv_obj_align(s_phase, LV_ALIGN_TOP_MID, 0, centre - 80);

    // The big digits have no colon, so it is drawn: two dots between them.
    s_clock = clear_box(card);
    lv_obj_set_size(s_clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_clock, 6, 0);
    s_minutes       = theme::make_label(s_clock, "25", theme::text, fonts::temp_64());
    lv_obj_t *colon = clear_box(s_clock);
    lv_obj_set_size(colon, COLON, 3 * COLON + 12);
    lv_obj_set_style_margin_top(colon, 12, 0);  // on the digits' middle, not their line's
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *dot = lv_obj_create(colon);
        theme::style_panel(dot, theme::text, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(dot, false);
        lv_obj_set_size(dot, COLON, COLON);
        lv_obj_align(dot, i == 0 ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    s_seconds = theme::make_label(s_clock, "00", theme::text, fonts::temp_64());
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, centre - 58);

    s_ends_name = theme::make_label(card, "ENDS AT", theme::secondary, fonts::size_16());
    lv_obj_align(s_ends_name, LV_ALIGN_TOP_MID, 0, centre + 24);
    s_ends = theme::make_accent_label(card, "--", fonts::size_32());
    lv_obj_align(s_ends, LV_ALIGN_TOP_MID, 0, centre + 46);

    const std::int32_t go_w = ring * 11 / 20;
    const std::int32_t go_h = 68;
    s_go                    = theme::make_button(card, "START", theme::panel);
    lv_obj_set_size(s_go, go_w, go_h);
    theme::fill_accent(s_go, LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_go, go_h / 2, 0);
    lv_obj_align(s_go, LV_ALIGN_TOP_MID, 0, ring_y + ring - go_h + 4);
    on_click(s_go, FocusAction::Toggle);
}

// What comes after this part, as the lights card shows whether they are on.
void build_next(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    theme::style_panel(card, theme::panel_light, theme::radius::card);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_scrollable(card, false);

    lv_obj_align(theme::make_label(card, "UP NEXT", theme::secondary, fonts::size_22()),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    s_next = theme::make_label(card, "--", theme::text, fonts::size_48());
    lv_obj_align(s_next, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_next_long = theme::make_label(card, "", theme::secondary, fonts::size_22());
    lv_obj_align(s_next_long, LV_ALIGN_BOTTOM_RIGHT, 0, -6);
}

// Which round of the set this is, as the speaker card says what is playing.
void build_rounds(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                  std::int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    theme::style_panel(card, theme::panel_light, theme::radius::card);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_set_scrollable(card, false);

    lv_obj_align(theme::make_label(card, "ROUND", theme::secondary, fonts::size_16()),
                 LV_ALIGN_TOP_LEFT, 0, 0);
    s_round = theme::make_label(card, "--", theme::text, fonts::size_20());
    lv_obj_align(s_round, LV_ALIGN_TOP_LEFT, 0, 28);

    lv_obj_t *dots = clear_box(card);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, ROUND_DOT);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 14, 0);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    for (lv_obj_t *&dot : s_round_dot) {
        dot = lv_obj_create(dots);
        theme::style_panel(dot, theme::panel, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(dot, false);
        lv_obj_set_size(dot, ROUND_DOT, ROUND_DOT);
    }
}

}  // namespace

void show_focus(const Focus &focus)
{
    s_focus   = focus;
    s_shown_s = -1;
    if (s_dial == nullptr) {
        return;
    }
    const bool idle    = focus.phase == FocusPhase::Idle;
    const bool paused  = !idle && !focus.running;
    const bool resting = focus.phase == FocusPhase::Break || focus.phase == FocusPhase::LongBreak;

    char text[48];
    std::snprintf(text, sizeof(text), "%s", paused ? "PAUSED" : phase_name(focus.phase));
    theme::set_text(s_phase, text);
    theme::arc_accent_or(s_dial, !resting, theme::green, LV_PART_INDICATOR);
    theme::fill_accent_or(s_dial, !resting, theme::green, LV_PART_KNOB);
    lv_obj_set_style_opa(s_clock, paused ? LV_OPA_60 : LV_OPA_COVER, 0);

    if (focus.running) {
        const std::time_t ends = std::time(nullptr) + (focus.ends_at_ms - now_ms() + 999) / 1000;
        std::tm           at{};
        localtime_r(&ends, &at);
        std::snprintf(text, sizeof(text), "%02d:%02d", at.tm_hour, at.tm_min);
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    theme::set_text(s_ends, text);

    theme::set_text(lv_obj_get_child(s_go, 0), idle ? "START" : focus.running ? "PAUSE" : "RESUME");
    lv_obj_set_state(s_go, LV_STATE_CHECKED, focus.running);
    enable(s_skip, !idle);
    enable(s_reset, !idle);

    // The plan, with the part under way lit.
    const int minutes[PILLS] = {focus.work_min, focus.break_min, focus.long_break_min};
    const FocusPhase phases[PILLS] = {FocusPhase::Work, FocusPhase::Break, FocusPhase::LongBreak};
    for (int i = 0; i < PILLS; ++i) {
        std::snprintf(text, sizeof(text), "%d min", minutes[i]);
        theme::set_text(s_pill[i].value, text);
        const bool on = focus.phase == phases[i];
        theme::fill_accent_or(s_pill[i].dot, on && i == 0, on ? theme::green : theme::secondary);
    }

    const bool last = focus.round >= focus.rounds;
    switch (focus.phase) {
        case FocusPhase::Idle:
        case FocusPhase::Break:
        case FocusPhase::LongBreak:
            theme::set_text(s_next, "Focus");
            std::snprintf(text, sizeof(text), "%d min", focus.work_min);
            break;
        case FocusPhase::Work:
            theme::set_text(s_next, last ? "Long break" : "Break");
            std::snprintf(text, sizeof(text), "%d min", last ? focus.long_break_min : focus.break_min);
            break;
    }
    theme::set_text(s_next_long, text);

    const int round = idle ? 1 : focus.round;
    std::snprintf(text, sizeof(text), "%d of %d", round, focus.rounds);
    theme::set_text(s_round, text);
    // A round is done once its focus has run; the one under way is outlined.
    const int done = focus.phase == FocusPhase::Work ? focus.round - 1
                     : idle                          ? 0
                                                     : focus.round;
    for (int i = 0; i < ROUNDS_MAX; ++i) {
        lv_obj_t *dot = s_round_dot[i];
        lv_obj_set_hidden(dot, i >= focus.rounds);
        theme::fill_accent_or(dot, i < done, theme::panel);
        lv_obj_set_style_border_color(dot, lv_color_hex(theme::primary), 0);
        lv_obj_set_style_border_width(dot, !idle && i == round - 1 && i >= done ? 3 : 0, 0);
    }

    show_time();
}

void build_focus_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    build_pills(page, width);

    const std::int32_t body_y = PILL_H + BUTTON_GAP;
    const std::int32_t body_h = height - body_y;
    build_dial(page, body_y, DIAL_W, body_h);

    const std::int32_t col_x  = DIAL_W + BUTTON_GAP;
    const std::int32_t col_w  = width - col_x;
    const std::int32_t next_h = body_h * 5 / 9;
    build_next(page, col_x, body_y, col_w, next_h);
    const std::int32_t rounds_y = body_y + next_h + BUTTON_GAP;
    build_rounds(page, col_x, rounds_y, col_w, height - rounds_y);

    lv_timer_create(timer_tick, 200, nullptr);
    show_focus(s_focus);
}

}  // namespace ui
