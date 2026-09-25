#include "focus_page.h"

#include "ui_internal.h"

#include "esp_timer.h"
#include "fonts/units_font.h"

#include <algorithm>
#include <ctime>
#include <cstdio>

// A dial of ticks, one for every minute of the part, round a flip clock's two
// leaves, its controls in the corners as the radar has its zoom. Beside it the
// set, laid out as the calendar lays out a journey: a stop for every part.
namespace ui {
namespace {
using detail::BUTTON_GAP;
using detail::PANEL_PAD;

constexpr std::int32_t TICK_W     = 24;
constexpr int          TICKS_MAX  = 90;
constexpr int          ROUNDS_MAX = 8;
constexpr int          PARTS_MAX  = 2 * ROUNDS_MAX;
constexpr std::int32_t CHIP       = theme::chip::size;
constexpr std::int32_t LEAF_W     = 176;
constexpr std::int32_t LEAF_H     = 150;
constexpr std::int32_t COLON      = 11;
constexpr std::int32_t TIME_W     = 64;
constexpr std::int32_t NODE       = 16;
constexpr std::int32_t RAIL       = 4;
constexpr std::int32_t SET_W      = 330;  // room for "Long break" beside its minutes

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
lv_obj_t *s_go         = nullptr;
lv_obj_t *s_skip       = nullptr;
lv_obj_t *s_reset      = nullptr;

struct Stop {  // a part of the set, or with the last, its end
    lv_obj_t *when = nullptr;
    lv_obj_t *node = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *mins = nullptr;
    lv_obj_t *rail = nullptr;  // on to the next stop
};
Stop         s_stop[PARTS_MAX + 1];
std::int32_t s_set_h    = 0;
int          s_laid_out = -1;  // how many parts the stops are laid out for

int s_shown_s = -1;

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

bool resting()
{
    return s_focus.phase == FocusPhase::Break || s_focus.phase == FocusPhase::LongBreak;
}

std::uint32_t ink_of(bool rest)
{
    return rest ? theme::green : theme::primary;
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

int parts()
{
    return std::clamp(2 * s_focus.rounds, 2, PARTS_MAX);
}

bool is_rest(int index)
{
    return index % 2 == 1;
}

int part_minutes(int index)
{
    if (index == parts() - 1) {
        return s_focus.long_break_min;
    }
    return is_rest(index) ? s_focus.break_min : s_focus.work_min;
}

const char *part_name(int index)
{
    if (index == parts() - 1) {
        return "Long break";
    }
    return is_rest(index) ? "Break" : "Focus";
}

// Where in the set the timer is: focus and break alternate, the last break long.
int part_index()
{
    switch (s_focus.phase) {
        case FocusPhase::Work:      return 2 * (s_focus.round - 1);
        case FocusPhase::Break:     return 2 * (s_focus.round - 1) + 1;
        case FocusPhase::LongBreak: return parts() - 1;
        case FocusPhase::Idle:      break;
    }
    return -1;
}

void clock_text(std::int64_t in_ms, char *out, std::size_t size)
{
    const std::time_t at =
        std::time(nullptr) + static_cast<std::time_t>((in_ms + (in_ms >= 0 ? 999 : 0)) / 1000);
    std::tm local{};
    localtime_r(&at, &local);
    std::snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
}

// Once a second: the leaves, the colon beating, and the minute under way
// breathing on the dial.
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
    const lv_color_t   ink     = lv_color_hex(ink_of(resting()));
    for (int i = 0; i < s_tick_count; ++i) {
        const bool lit     = i < done;
        const bool current = !idle && i == done;
        lv_obj_set_style_arc_color(s_tick[i], lit || current ? ink : lv_color_hex(theme::panel),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_opa(s_tick[i],
                                 current ? (s_focus.running && !beat ? LV_OPA_20 : LV_OPA_50)
                                         : LV_OPA_COVER,
                                 LV_PART_MAIN);
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
    lv_obj_set_scrollable(box, false);
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
    count            = std::clamp(count, 1, TICKS_MAX);
    s_tick_count     = count;
    const float step = 360.0f / static_cast<float>(count);
    const float gap  = count > 40 ? 1.5f : count > 20 ? 2.5f : 3.0f;
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

lv_obj_t *leaf(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    theme::style_panel(root, theme::panel, theme::radius::control);
    lv_obj_set_clickable(root, false);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_size(root, LEAF_W, LEAF_H);
    lv_obj_t *digits = theme::make_label(root, "00", theme::text, fonts::clock_104());
    lv_obj_center(digits);
    lv_obj_t *fold = lv_obj_create(root);
    theme::style_panel(fold, theme::panel_light, 0);
    lv_obj_set_clickable(fold, false);
    lv_obj_set_size(fold, LEAF_W, 4);
    lv_obj_align(fold, LV_ALIGN_LEFT_MID, 0, 0);
    return digits;
}

void build_dial(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = card_at(parent, 0, 0, w, h);
    lv_obj_set_style_pad_all(card, PANEL_PAD, 0);
    const std::int32_t inner   = w - 2 * PANEL_PAD;
    const std::int32_t inner_h = h - 2 * PANEL_PAD;

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

    // Minutes and seconds each on a leaf, cut across where a flip clock's
    // leaves fold, with a colon that beats the seconds between them. The clock
    // is a button too: the biggest thing on the page is the obvious one to tap.
    s_leaves = clear_box(card);
    lv_obj_set_size(s_leaves, LV_SIZE_CONTENT, LEAF_H);
    lv_obj_set_flex_flow(s_leaves, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_leaves, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_leaves, 16, 0);
    lv_obj_center(s_leaves);
    lv_obj_set_clickable(s_leaves, true);
    on_click(s_leaves, FocusAction::Toggle);
    s_minutes       = leaf(s_leaves);
    lv_obj_t *colon = clear_box(s_leaves);
    lv_obj_set_size(colon, COLON, 3 * COLON + 18);
    for (int i = 0; i < 2; ++i) {
        s_colon[i] = lv_obj_create(colon);
        theme::style_panel(s_colon[i], theme::secondary, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(s_colon[i], false);
        lv_obj_set_size(s_colon[i], COLON, COLON);
        lv_obj_align(s_colon[i], i == 0 ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    s_seconds = leaf(s_leaves);

    s_phase = theme::make_eyebrow(card, "READY");
    lv_obj_align(s_phase, LV_ALIGN_CENTER, 0, -LEAF_H / 2 - 34);
    s_ends = theme::make_label(card, "", theme::secondary, theme::type_body());
    lv_obj_align(s_ends, LV_ALIGN_CENTER, 0, LEAF_H / 2 + 34);

    // The corners, as the radar's: what to do in three, a screw in the fourth.
    s_reset = theme::make_chip(card, LV_SYMBOL_REFRESH);
    lv_obj_set_pos(s_reset, 0, 0);
    on_click(s_reset, FocusAction::Reset);
    s_skip = theme::make_chip(card, LV_SYMBOL_NEXT);
    lv_obj_set_pos(s_skip, inner - CHIP, 0);
    on_click(s_skip, FocusAction::Skip);
    lv_obj_set_pos(theme::make_screw(card, CHIP), 0, inner_h - CHIP);
    s_go = theme::make_chip(card, LV_SYMBOL_PLAY);
    theme::fill_accent(s_go, LV_STATE_CHECKED);
    lv_obj_set_pos(s_go, inner - CHIP, inner_h - CHIP);
    on_click(s_go, FocusAction::Toggle);
    for (lv_obj_t *chip : {s_reset, s_skip, s_go}) {
        lv_obj_set_ext_click_area(chip, theme::space::s);
    }
}

void build_set(lv_obj_t *parent, std::int32_t x, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = card_at(parent, x, 0, w, h);
    lv_obj_set_style_pad_all(card, theme::space::l, 0);
    const std::int32_t inner   = w - 2 * theme::space::l;
    const std::int32_t inner_h = h - 2 * theme::space::l;

    lv_obj_align(theme::make_eyebrow(card, "THE SET"), LV_ALIGN_TOP_LEFT, 0, 0);

    const std::int32_t top = theme::type_label()->line_height + theme::space::l;
    lv_obj_t          *set = clear_box(card);
    s_set_h                = inner_h - top;
    lv_obj_set_pos(set, 0, top);
    lv_obj_set_size(set, inner, s_set_h);

    const std::int32_t rail_x = TIME_W + theme::space::s + NODE / 2;
    const std::int32_t text_x = rail_x + NODE / 2 + theme::space::m;
    for (Stop &stop : s_stop) {  // the rails first, so the nodes sit on them
        stop.rail = clear_box(set);
        lv_obj_set_style_bg_opa(stop.rail, LV_OPA_COVER, 0);
        lv_obj_set_width(stop.rail, RAIL);
        lv_obj_set_x(stop.rail, rail_x - RAIL / 2);
    }
    for (Stop &stop : s_stop) {
        stop.when = theme::make_label(set, "", theme::text, theme::type_body());
        lv_obj_set_width(stop.when, TIME_W);
        stop.node = lv_obj_create(set);
        theme::style_panel(stop.node, theme::panel_light, LV_RADIUS_CIRCLE);
        lv_obj_set_clickable(stop.node, false);
        lv_obj_set_size(stop.node, NODE, NODE);
        lv_obj_set_style_border_width(stop.node, 3, 0);
        lv_obj_set_x(stop.node, rail_x - NODE / 2);
        stop.name = theme::make_label(set, "", theme::text, theme::type_body());
        lv_obj_set_x(stop.name, text_x);
        stop.mins = theme::make_label(set, "", theme::secondary, theme::type_label());
        lv_obj_set_width(stop.mins, inner - text_x);
        lv_obj_set_style_text_align(stop.mins, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_x(stop.mins, text_x);
    }
}

// The stops, as many as the set has parts and one for its end, spread down
// the card; when each part starts while the time runs, and how far it has got.
void show_set()
{
    const int          count = parts();
    const std::int32_t line  = theme::type_body()->line_height;
    const std::int32_t pitch = (s_set_h - line) / count;
    if (s_laid_out != count) {
        s_laid_out = count;
        for (int i = 0; i <= PARTS_MAX; ++i) {
            Stop      &stop = s_stop[i];
            const bool real = i <= count;
            for (lv_obj_t *obj : {stop.when, stop.node, stop.name, stop.mins, stop.rail}) {
                lv_obj_set_hidden(obj, !real);
            }
            if (!real) {
                continue;
            }
            const std::int32_t y = i * pitch;
            lv_obj_set_y(stop.when, y);
            lv_obj_set_y(stop.node, y + (line - NODE) / 2);
            lv_obj_set_y(stop.name, y);
            lv_obj_set_y(stop.mins, y + (line - theme::type_label()->line_height) / 2);
            lv_obj_set_y(stop.rail, y + line / 2);
            lv_obj_set_height(stop.rail, pitch);
            lv_obj_set_hidden(stop.rail, i == count);
        }
    }

    const int  at      = part_index();
    const bool running = s_focus.running && at >= 0;
    // When each part starts: the one under way began its length before it
    // ends, and each after it follows on.
    std::int64_t starts_in = running ? s_focus.ends_at_ms - now_ms() - s_focus.length_ms : 0;
    char         text[24];
    for (int i = 0; i <= count; ++i) {
        Stop      &stop = s_stop[i];
        const bool end  = i == count;
        const bool done = at >= 0 && i < at;
        const bool now  = i == at;
        const auto ink  = ink_of(end || is_rest(i));

        if (running && i >= at) {
            clock_text(starts_in, text, sizeof(text));
            theme::set_text(stop.when, text);
            if (!end) {
                starts_in += now ? s_focus.length_ms
                                 : static_cast<std::int64_t>(part_minutes(i)) * 60000;
            }
        } else {
            theme::set_text(stop.when, "");
        }
        theme::set_text(stop.name, end ? "Set done" : part_name(i));
        if (end) {
            theme::set_text(stop.mins, "");
        } else {
            std::snprintf(text, sizeof(text), "%d min", part_minutes(i));
            theme::set_text(stop.mins, text);
        }
        theme::set_text_color(stop.name, now ? ink : theme::text);

        lv_obj_set_style_bg_color(stop.node, lv_color_hex(done || now ? ink : theme::panel_light),
                                  0);
        lv_obj_set_style_border_color(stop.node, lv_color_hex(done || now ? ink : theme::secondary),
                                      0);
        lv_obj_set_style_bg_color(
            stop.rail,
            done || now ? lv_color_hex(ink)
                        : lv_color_mix(lv_color_hex(ink), lv_color_hex(theme::panel_light), 70),
            0);
        for (lv_obj_t *obj : {stop.when, stop.name, stop.mins}) {
            lv_obj_set_style_opa(obj, done ? LV_OPA_40 : LV_OPA_COVER, 0);
        }
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
                          focus.length_ms != s_focus.length_ms ||
                          focus.work_min != s_focus.work_min || s_tick_count == 0;
    s_focus = focus;
    detail::paint_focus_plan(focus);
    if (s_leaves == nullptr) {
        return;
    }
    const bool idle   = focus.phase == FocusPhase::Idle;
    const bool paused = !idle && !focus.running;

    if (new_part) {
        lay_ticks(idle ? focus.work_min : std::max<int>(1, focus.length_ms / 60000));
    }

    char text[48];
    std::snprintf(text, sizeof(text), "%s%s", phase_name(focus.phase), paused ? ", PAUSED" : "");
    theme::set_text(s_phase, text);
    if (focus.running) {
        char when[16];
        clock_text(focus.ends_at_ms - now_ms(), when, sizeof(when));
        std::snprintf(text, sizeof(text), "round %d of %d, until %s", focus.round, focus.rounds,
                      when);
    } else if (paused) {
        std::snprintf(text, sizeof(text), "round %d of %d, paused", focus.round, focus.rounds);
    } else {
        std::snprintf(text, sizeof(text), "%d rounds, tap to start", focus.rounds);
    }
    theme::set_text(s_ends, text);

    lv_obj_set_state(s_go, LV_STATE_CHECKED, focus.running);
    lv_obj_t *glyph = lv_obj_get_child(s_go, 0);
    theme::set_text(glyph, focus.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    theme::set_text_color(glyph, focus.running ? theme::text : theme::secondary);
    theme::center_ink(glyph);
    theme::set_usable(s_skip, !idle);
    theme::set_usable(s_reset, !idle);

    show_set();
    show_second(true);
}

void build_focus_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    const std::int32_t dial_w = width - SET_W - BUTTON_GAP;
    build_dial(page, dial_w, height);
    build_set(page, dial_w + BUTTON_GAP, SET_W, height);

    lv_timer_create(timer_tick, 250, nullptr);
    show_focus(s_focus);
}

}  // namespace ui
