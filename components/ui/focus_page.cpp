#include "focus_page.h"

#include "ui_internal.h"

#include "esp_timer.h"
#include "fonts/units_font.h"

#include <algorithm>
#include <ctime>
#include <cstdio>

// A dial of ticks, one for every minute of the part, around a flip clock's two
// leaves, and under it the whole set as the calendar draws a day.
namespace ui {
namespace {
using detail::BUTTON_GAP;
using detail::PANEL_PAD;

constexpr std::int32_t DIAL_W     = 500;
constexpr std::int32_t STRIP_H    = 116;
constexpr std::int32_t TICK_W     = 22;
constexpr int          TICKS_MAX  = 60;
constexpr int          ROUNDS_MAX = 8;
constexpr int          PARTS_MAX  = 2 * ROUNDS_MAX;
constexpr std::int32_t ROUND_DOT  = 22;
constexpr std::int32_t CHIP       = theme::chip::size;
constexpr std::int32_t LEAF_W     = 136;
constexpr std::int32_t LEAF_H     = 118;
constexpr std::int32_t COLON      = 9;

// As last told; until then, the plan as it stands at boot.
Focus s_focus{FocusPhase::Idle, 0, 4, false, 0, 0, 0, 25, 5, 20};

lv_obj_t *s_tick[TICKS_MAX];
int       s_tick_count = 0;
lv_obj_t *s_leaves     = nullptr;  // the two leaves and the colon between them
lv_obj_t *s_minutes    = nullptr;
lv_obj_t *s_seconds    = nullptr;
lv_obj_t *s_colon[2]   = {};
lv_obj_t *s_phase      = nullptr;
lv_obj_t *s_ends       = nullptr;

lv_obj_t *s_go       = nullptr;  // the card that starts and pauses
lv_obj_t *s_go_round = nullptr;
lv_obj_t *s_go_word  = nullptr;
lv_obj_t *s_go_dot[ROUNDS_MAX];

lv_obj_t *s_next  = nullptr;
lv_obj_t *s_skip  = nullptr;
lv_obj_t *s_reset = nullptr;

struct Part {
    lv_obj_t *root = nullptr;
    lv_obj_t *fill = nullptr;
    lv_obj_t *mins = nullptr;
    std::int32_t w = 0;
};
Part         s_part[PARTS_MAX];
lv_obj_t    *s_done_at = nullptr;
std::int32_t s_track_w = 0;

int s_shown_s = -1;

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

bool resting()
{
    return s_focus.phase == FocusPhase::Break || s_focus.phase == FocusPhase::LongBreak;
}

std::uint32_t ink()
{
    return resting() ? theme::green : theme::primary;
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

int part_minutes(int index)
{
    if (index == 2 * s_focus.rounds - 1) {
        return s_focus.long_break_min;
    }
    return index % 2 == 0 ? s_focus.work_min : s_focus.break_min;
}

// Where in the set the timer is: focus and break alternate, the last break long.
int part_index()
{
    switch (s_focus.phase) {
        case FocusPhase::Work:      return 2 * (s_focus.round - 1);
        case FocusPhase::Break:     return 2 * (s_focus.round - 1) + 1;
        case FocusPhase::LongBreak: return 2 * s_focus.rounds - 1;
        case FocusPhase::Idle:      break;
    }
    return -1;
}

void clock_text(std::int64_t in_ms, char *out, std::size_t size)
{
    const std::time_t at = std::time(nullptr) + static_cast<std::time_t>((in_ms + 999) / 1000);
    std::tm           local{};
    localtime_r(&at, &local);
    std::snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
}

// Once a second: the figures, the colon beating, the minute under way breathing
// on the dial, and the part under way filling on the strip.
void show_second(bool force)
{
    const std::int32_t left    = left_ms();
    const int          seconds = static_cast<int>((left + 999) / 1000);
    if (seconds == s_shown_s && !force) {
        return;
    }
    s_shown_s = seconds;

    const bool idle   = s_focus.phase == FocusPhase::Idle;
    const bool paused = !idle && !s_focus.running;
    const bool beat   = seconds % 2 == 0;
    char       text[8];
    std::snprintf(text, sizeof(text), "%02d", seconds / 60);
    theme::set_text(s_minutes, text);
    std::snprintf(text, sizeof(text), "%02d", seconds % 60);
    theme::set_text(s_seconds, text);
    for (lv_obj_t *dot : s_colon) {
        lv_obj_set_style_bg_opa(dot, !s_focus.running || beat ? LV_OPA_COVER : LV_OPA_20, 0);
    }
    lv_obj_set_style_opa(s_leaves, paused && !beat ? LV_OPA_40 : LV_OPA_COVER, 0);

    const std::int32_t length  = idle ? 0 : s_focus.length_ms;
    const std::int32_t elapsed = length > 0 ? length - left : 0;
    const int          done    = length > 0 ? static_cast<int>(elapsed / 60000) : 0;
    for (int i = 0; i < s_tick_count; ++i) {
        const bool lit     = i < done;
        const bool current = !idle && i == done;
        const lv_color_t colour =
            lit || current ? lv_color_hex(ink()) : lv_color_hex(theme::panel);
        lv_obj_set_style_arc_color(s_tick[i], colour, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(s_tick[i],
                                 current ? (s_focus.running && !beat ? LV_OPA_20 : LV_OPA_50)
                                         : LV_OPA_COVER,
                                 LV_PART_MAIN);
    }

    const int at = part_index();
    if (at >= 0 && length > 0) {
        const Part &part = s_part[at];
        lv_obj_set_width(part.fill, static_cast<std::int32_t>(
                                        static_cast<std::int64_t>(part.w) * elapsed / length));
    }
}

void timer_tick(lv_timer_t *)
{
    if (detail::s_page == detail::FOCUS_PAGE && s_leaves != nullptr) {
        show_second(false);
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

lv_obj_t *card_at(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    theme::style_panel(card, theme::panel_light, theme::radius::card);
    lv_obj_set_scrollable(card, false);
    return card;
}

// The ticks go round from the top, a minute each: a focus part's twenty-five
// are fine, a break's five are fat.
void lay_ticks(int count)
{
    count          = std::clamp(count, 1, TICKS_MAX);
    s_tick_count   = count;
    const float step = 360.0f / static_cast<float>(count);
    const float gap  = count > 30 ? 2.0f : 3.0f;
    for (int i = 0; i < TICKS_MAX; ++i) {
        lv_obj_set_hidden(s_tick[i], i >= count);
        if (i >= count) {
            continue;
        }
        const auto from = static_cast<int>(270.0f + step * static_cast<float>(i) + gap / 2.0f);
        const auto to   = static_cast<int>(270.0f + step * static_cast<float>(i + 1) - gap / 2.0f);
        lv_arc_set_bg_angles(s_tick[i], static_cast<lv_value_precise_t>(from % 360),
                             static_cast<lv_value_precise_t>(to % 360));
    }
}

void build_dial(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = card_at(parent, 0, 0, w, h);
    lv_obj_set_style_pad_all(card, PANEL_PAD, 0);
    const std::int32_t inner   = w - 2 * PANEL_PAD;
    const std::int32_t inner_h = h - 2 * PANEL_PAD;
    lv_obj_set_pos(theme::make_screw(card, CHIP), 0, 0);
    lv_obj_set_pos(theme::make_screw(card, CHIP), inner - CHIP, 0);
    lv_obj_set_pos(theme::make_screw(card, CHIP), 0, inner_h - CHIP);
    lv_obj_set_pos(theme::make_screw(card, CHIP), inner - CHIP, inner_h - CHIP);

    const std::int32_t ring = std::min(inner, inner_h) - 8;
    for (lv_obj_t *&tick : s_tick) {
        tick = lv_arc_create(card);
        lv_obj_set_size(tick, ring, ring);
        lv_obj_center(tick);
        lv_arc_set_rotation(tick, 0);
        lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(tick, TICK_W, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(tick, false, LV_PART_MAIN);
        lv_obj_set_style_arc_color(tick, lv_color_hex(theme::panel), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(tick, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(tick, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(tick, 0, LV_PART_KNOB);
    }

    // Minutes and seconds each on a leaf, cut across the middle where a flip
    // clock's leaves fold, with a colon that beats the seconds between them.
    s_leaves = clear_box(card);
    lv_obj_set_size(s_leaves, LV_SIZE_CONTENT, LEAF_H);
    lv_obj_set_flex_flow(s_leaves, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_leaves, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_leaves, 14, 0);
    lv_obj_center(s_leaves);
    const auto leaf = [](lv_obj_t *parent) {
        lv_obj_t *root = lv_obj_create(parent);
        theme::style_panel(root, theme::panel, theme::radius::control);
        lv_obj_set_clickable(root, false);
        lv_obj_set_size(root, LEAF_W, LEAF_H);
        lv_obj_t *digits = theme::make_label(root, "00", theme::text, fonts::temp_64());
        lv_obj_center(digits);
        lv_obj_t *fold = lv_obj_create(root);
        theme::style_panel(fold, theme::panel_light, 0);
        lv_obj_set_clickable(fold, false);
        lv_obj_set_size(fold, LEAF_W, 3);
        lv_obj_align(fold, LV_ALIGN_LEFT_MID, 0, 0);
        return digits;
    };
    s_minutes       = leaf(s_leaves);
    lv_obj_t *colon = clear_box(s_leaves);
    lv_obj_set_size(colon, COLON, 3 * COLON + 14);
    for (int i = 0; i < 2; ++i) {
        s_colon[i] = lv_obj_create(colon);
        theme::style_panel(s_colon[i], theme::secondary, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(s_colon[i], false);
        lv_obj_set_size(s_colon[i], COLON, COLON);
        lv_obj_align(s_colon[i], i == 0 ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    s_seconds = leaf(s_leaves);

    s_phase = theme::make_eyebrow(card, "READY");
    lv_obj_align(s_phase, LV_ALIGN_CENTER, 0, -LEAF_H / 2 - 30);
    s_ends = theme::make_label(card, "", theme::secondary, theme::type_body());
    lv_obj_align(s_ends, LV_ALIGN_CENTER, 0, LEAF_H / 2 + 30);
}

// The whole card is the button, lit while the time runs, as the lights card
// is while they are on.
void build_go(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
{
    s_go = lv_button_create(parent);
    lv_obj_set_pos(s_go, x, y);
    lv_obj_set_size(s_go, w, h);
    theme::style_button(s_go, theme::panel_light);
    theme::fill_accent(s_go, LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_go, theme::radius::card, 0);
    lv_obj_set_style_pad_all(s_go, 28, 0);
    on_click(s_go, FocusAction::Toggle);

    s_go_round = theme::make_label(s_go, "", theme::secondary, fonts::size_22());
    lv_obj_align(s_go_round, LV_ALIGN_TOP_LEFT, 0, 0);
    s_go_word = theme::make_label(s_go, "Start", theme::text, fonts::size_48());
    lv_obj_align(s_go_word, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *dots = clear_box(s_go);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, ROUND_DOT);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 10, 0);
    lv_obj_align(dots, LV_ALIGN_BOTTOM_RIGHT, 0, -12);
    for (lv_obj_t *&dot : s_go_dot) {
        dot = lv_obj_create(dots);
        theme::style_panel(dot, theme::panel, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(dot, false);
        lv_obj_set_size(dot, ROUND_DOT, ROUND_DOT);
    }
}

void build_next(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = card_at(parent, x, y, w, h);
    lv_obj_set_style_pad_all(card, theme::space::l, 0);
    const std::int32_t inner   = w - 2 * theme::space::l;
    const std::int32_t inner_h = h - 2 * theme::space::l;

    lv_obj_align(theme::make_eyebrow(card, "UP NEXT"), LV_ALIGN_TOP_LEFT, 0, 0);
    s_next = theme::make_label(card, "", theme::text, theme::type_title());
    lv_obj_set_width(s_next, inner - 2 * CHIP - 2 * theme::space::s);
    lv_label_set_long_mode(s_next, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_next, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_skip = theme::make_chip(card, LV_SYMBOL_NEXT);
    lv_obj_set_pos(s_skip, inner - CHIP, inner_h - CHIP);
    on_click(s_skip, FocusAction::Skip);
    s_reset = theme::make_chip(card, LV_SYMBOL_REFRESH);
    lv_obj_set_pos(s_reset, inner - 2 * CHIP - theme::space::s, inner_h - CHIP);
    on_click(s_reset, FocusAction::Reset);
}

void build_strip(lv_obj_t *parent, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = card_at(parent, 0, y, w, h);
    lv_obj_set_style_pad_all(card, PANEL_PAD + 4, 0);
    const std::int32_t inner = w - 2 * (PANEL_PAD + 4);

    lv_obj_align(theme::make_eyebrow(card, "THE SET"), LV_ALIGN_TOP_LEFT, 0, 0);
    s_done_at = theme::make_label(card, "", theme::secondary, theme::type_label());
    lv_obj_align(s_done_at, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *track = clear_box(card);
    const std::int32_t track_y = theme::type_label()->line_height + theme::space::s;
    lv_obj_set_pos(track, 0, track_y);
    lv_obj_set_size(track, inner, h - 2 * (PANEL_PAD + 4) - track_y);
    s_track_w = inner;
    for (Part &part : s_part) {
        part.root = lv_obj_create(track);
        theme::style_panel(part.root, theme::panel, theme::radius::row / 2);
        lv_obj_set_clickable(part.root, false);
        lv_obj_set_height(part.root, LV_PCT(100));
        part.fill = lv_obj_create(part.root);
        theme::style_panel(part.fill, theme::primary, theme::radius::row / 2);
        lv_obj_set_clickable(part.fill, false);
        lv_obj_set_height(part.fill, LV_PCT(100));
        lv_obj_set_width(part.fill, 0);
        part.mins = theme::make_label(part.root, "", theme::text, theme::type_label());
        lv_obj_center(part.mins);
    }
}

// The strip's parts, as long as they last, and how far the set has got.
void show_strip()
{
    const int parts = std::min(2 * s_focus.rounds, PARTS_MAX);
    int       total = 0;
    for (int i = 0; i < parts; ++i) {
        total += part_minutes(i);
    }
    const int          at   = part_index();
    const std::int32_t gaps = (parts - 1) * 4;
    std::int32_t       x    = 0;
    char               text[32];
    for (int i = 0; i < PARTS_MAX; ++i) {
        Part &part = s_part[i];
        lv_obj_set_hidden(part.root, i >= parts);
        if (i >= parts) {
            continue;
        }
        const int          mins = part_minutes(i);
        const std::int32_t w =
            i == parts - 1 ? s_track_w - x
                           : (s_track_w - gaps) * mins / std::max(total, 1);
        part.w = w;
        lv_obj_set_pos(part.root, x, 0);
        lv_obj_set_width(part.root, w);
        x += w + 4;

        const bool     rest   = i % 2 == 1;
        const auto     colour = rest ? theme::green : theme::primary;
        const bool     done   = at >= 0 && i < at;
        lv_obj_set_style_bg_color(part.root,
                                  lv_color_mix(lv_color_hex(colour), lv_color_hex(theme::panel),
                                               done ? 255 : 70),
                                  0);
        lv_obj_set_style_bg_color(part.fill, lv_color_hex(colour), 0);
        if (i != at) {
            lv_obj_set_width(part.fill, 0);
        }
        std::snprintf(text, sizeof(text), "%d", mins);
        theme::set_text(part.mins, text);
        lv_obj_set_hidden(part.mins, w < 26);
    }

    if (s_focus.running && at >= 0) {
        std::int64_t in = s_focus.ends_at_ms - now_ms();
        for (int i = at + 1; i < parts; ++i) {
            in += static_cast<std::int64_t>(part_minutes(i)) * 60000;
        }
        char when[16];
        clock_text(in, when, sizeof(when));
        std::snprintf(text, sizeof(text), "done at %s", when);
        theme::set_text(s_done_at, text);
    } else {
        std::snprintf(text, sizeof(text), "%d min", total);
        theme::set_text(s_done_at, text);
    }
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

}  // namespace

void show_focus(const Focus &focus)
{
    const bool new_part = focus.phase != s_focus.phase || focus.round != s_focus.round ||
                          s_tick_count == 0;
    s_focus = focus;
    if (s_leaves == nullptr) {
        return;
    }
    const bool idle   = focus.phase == FocusPhase::Idle;
    const bool paused = !idle && !focus.running;

    if (new_part) {
        const int minutes = idle ? focus.work_min : std::max<int>(1, focus.length_ms / 60000);
        lay_ticks(minutes);
    }

    char text[48];
    std::snprintf(text, sizeof(text), "%s%s", phase_name(focus.phase), paused ? ", PAUSED" : "");
    theme::set_text(s_phase, text);
    if (focus.running) {
        char when[16];
        clock_text(focus.ends_at_ms - now_ms(), when, sizeof(when));
        std::snprintf(text, sizeof(text), "until %s", when);
    } else {
        std::snprintf(text, sizeof(text), "%s", idle ? "a set is four rounds" : "tap to carry on");
    }
    theme::set_text(s_ends, text);

    lv_obj_set_state(s_go, LV_STATE_CHECKED, focus.running);
    theme::set_text(s_go_word, idle ? "Start" : focus.running ? "Pause" : "Resume");
    std::snprintf(text, sizeof(text), "ROUND %d OF %d", idle ? 1 : focus.round, focus.rounds);
    theme::set_text(s_go_round, text);
    theme::set_text_color(s_go_round, focus.running ? theme::text : theme::secondary);
    const int done = focus.phase == FocusPhase::Work ? focus.round - 1 : idle ? 0 : focus.round;
    for (int i = 0; i < ROUNDS_MAX; ++i) {
        lv_obj_t *dot = s_go_dot[i];
        lv_obj_set_hidden(dot, i >= focus.rounds);
        const bool filled = i < done;
        lv_obj_set_style_bg_color(dot,
                                  lv_color_hex(filled ? (focus.running ? theme::text : theme::primary)
                                                      : theme::panel),
                                  0);
        lv_obj_set_style_bg_opa(dot, filled || !focus.running ? LV_OPA_COVER : LV_OPA_30, 0);
    }

    const bool last = focus.round >= focus.rounds;
    switch (focus.phase) {
        case FocusPhase::Work:
            std::snprintf(text, sizeof(text), last ? "Long break, %d min" : "Break, %d min",
                          last ? focus.long_break_min : focus.break_min);
            break;
        case FocusPhase::Break:
            std::snprintf(text, sizeof(text), "Round %d, %d min", focus.round + 1, focus.work_min);
            break;
        default:
            std::snprintf(text, sizeof(text), "Round 1, %d min", focus.work_min);
            break;
    }
    theme::set_text(s_next, text);
    theme::set_usable(s_skip, !idle);
    theme::set_usable(s_reset, !idle);

    show_strip();
    show_second(true);
}

void build_focus_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    const std::int32_t upper_h = height - STRIP_H - BUTTON_GAP;
    build_dial(page, DIAL_W, upper_h);

    const std::int32_t col_x = DIAL_W + BUTTON_GAP;
    const std::int32_t col_w = width - col_x;
    const std::int32_t go_h  = upper_h * 3 / 5;
    build_go(page, col_x, 0, col_w, go_h);
    build_next(page, col_x, go_h + BUTTON_GAP, col_w, upper_h - go_h - BUTTON_GAP);
    build_strip(page, upper_h + BUTTON_GAP, width, STRIP_H);

    lv_timer_create(timer_tick, 250, nullptr);
    show_focus(s_focus);
}

}  // namespace ui
