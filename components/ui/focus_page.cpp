#include "focus_page.h"

#include "focus_model.h"
#include "popout.h"
#include "topics.h"
#include "ui_internal.h"

#include "fonts/units_font.h"

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <utility>

// The timer fullscreen: a dial of ticks, one for every minute of the part,
// round a flip clock's two leaves, with nothing else but how far it has got.
// And a small card dropping from the top row's badge, with the time, how far
// the part is, and what to do.
namespace ui {
namespace {
constexpr int TICKS_MAX  = detail::FOCUS_WORK_MIN_MAX;  // no part runs longer
constexpr int ROUNDS_MAX = detail::FOCUS_ROUNDS_MAX;

constexpr std::uint32_t TIMER_PERIOD_MS = 250;

constexpr std::int32_t TICK_W           = 24;
constexpr int          DEGREES_PER_TURN = 360;
constexpr float        DIAL_TOP_DEGREES = 270.0f;  // LVGL counts from three o'clock

constexpr std::int32_t LEAF_W             = 176;
constexpr std::int32_t LEAF_H             = 150;
constexpr std::int32_t LEAF_GAP           = theme::space::m;
constexpr std::int32_t FOLD_H             = 4;
constexpr std::int32_t COLON_DOT          = 11;
constexpr std::int32_t COLON_DOT_GAP      = 29;
constexpr std::int32_t COLON_H            = 2 * COLON_DOT + COLON_DOT_GAP;
constexpr std::int32_t PHASE_ABOVE_LEAVES = 34;

constexpr std::int32_t FULL_PAD          = 64;  // the ring from the screen's top and bottom, clear of the row over it
constexpr std::int32_t FULL_DOT          = 10;
constexpr std::int32_t FULL_DOT_GAP      = 12;
constexpr std::int32_t FULL_ROUNDS_BELOW = 44;  // the rounds under the line under the leaves

// The model's, as last drawn: what changed is drawn against it.
Focus s_focus = detail::focus_state();

// A clock face: a tick for every minute round two leaves, and the colon that
// beats between them.
struct Face {
    lv_obj_t *ring            = nullptr;  // all of it to tap, or swipe on
    lv_obj_t *tick[TICKS_MAX] = {};
    lv_obj_t *leaves          = nullptr;
    lv_obj_t *minutes         = nullptr;
    lv_obj_t *seconds         = nullptr;
    lv_obj_t *colon[2]        = {};
};
Face s_full_face;
int  s_tick_count = 0;

lv_obj_t *s_full          = nullptr;  // the timer fullscreen
detail::ViewId s_full_view = detail::kNoView;
lv_obj_t *s_full_phase    = nullptr;
lv_obj_t *s_full_under    = nullptr;  // until when, or what a tap does
lv_obj_t *s_full_round[ROUNDS_MAX] = {};
int       s_full_shown_s  = -1;

using detail::whole_seconds_up;

bool resting()
{
    return detail::focus_resting(s_focus);
}

bool waiting()
{
    return detail::focus_waiting(s_focus);
}

std::uint32_t ink_of(bool rest)
{
    return detail::focus_ink(rest);
}

std::int32_t left_ms()
{
    return detail::focus_left_ms(s_focus);
}

void clock_text(std::int64_t in_ms, char *out, std::size_t size)
{
    const std::int64_t in_s = in_ms >= 0 ? whole_seconds_up(in_ms) : in_ms / units::kMsPerSecond;
    const std::time_t  at   = std::time(nullptr) + static_cast<std::time_t>(in_s);
    std::tm local{};
    localtime_r(&at, &local);
    std::snprintf(out, size, "%02d:%02d", local.tm_hour, local.tm_min);
}

void show_leaves(const Face &face, int seconds, bool paused, bool beat)
{
    char text[12];
    std::snprintf(text, sizeof(text), "%02d", seconds / units::kSecondsPerMinute);
    theme::set_text(face.minutes, text);
    std::snprintf(text, sizeof(text), "%02d", seconds % units::kSecondsPerMinute);
    theme::set_text(face.seconds, text);
    for (lv_obj_t *dot : face.colon) {
        lv_obj_set_style_bg_opa(dot, !s_focus.running || beat ? LV_OPA_COVER : LV_OPA_20, 0);
    }
    // Paused, faint and still: the face is drawn again only as the seconds change.
    lv_obj_set_style_opa(face.leaves, paused ? LV_OPA_40 : LV_OPA_COVER, 0);
}

void show_ticks(const Face &face, int done, bool idle, bool beat)
{
    const lv_color_t ink = lv_color_hex(ink_of(resting()));
    for (int i = 0; i < s_tick_count; ++i) {
        const bool lit     = i < done;
        const bool current = !idle && i == done;
        lv_obj_set_style_arc_color(face.tick[i], lit || current ? ink : lv_color_hex(theme::panel),
                                   LV_PART_MAIN);
        lv_obj_set_style_arc_opa(face.tick[i],
                                 current ? (s_focus.running && !beat ? LV_OPA_20 : LV_OPA_50)
                                         : LV_OPA_COVER,
                                 LV_PART_MAIN);
    }
}

void show_full_second(bool force);
void show_pop_second(bool force);
detail::Popout s_pop;

void timer_tick(lv_timer_t *)
{
    if (detail::view_open(s_full_view)) {
        show_full_second(false);
    }
    if (s_pop.open) {
        show_pop_second(false);
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

// A tap starts or pauses the part, a swipe to the left goes on to the next; the
// release that ends a swipe is not also a tap.
bool s_swiped = false;

void face_touched(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        s_swiped = true;
        if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_LEFT && detail::s_handlers.focus != nullptr) {
            detail::s_handlers.focus(FocusAction::Skip);
        }
        return;
    }
    if (!std::exchange(s_swiped, false) && detail::s_handlers.focus != nullptr) {
        detail::s_handlers.focus(FocusAction::Toggle);
    }
}

void touch_face(lv_obj_t *obj)
{
    lv_obj_set_clickable(obj, true);
    lv_obj_add_event_cb(obj, face_touched, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(obj, face_touched, LV_EVENT_GESTURE, nullptr);
    // Gestures bubble by default, past here to the screen.
    lv_obj_set_gesture_bubble(obj, false);
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

lv_obj_t *make_dot(lv_obj_t *parent, std::uint32_t colour, std::int32_t size)
{
    lv_obj_t *dot = lv_obj_create(parent);
    theme::style_panel(dot, colour, LV_RADIUS_CIRCLE);
    lv_obj_set_clickable(dot, false);
    lv_obj_set_size(dot, size, size);
    return dot;
}

// Fewer ticks are fatter, and stand further apart to still read as ticks.
float tick_gap_degrees(int count)
{
    constexpr int   MANY_TICKS = 40;
    constexpr int   SOME_TICKS = 20;
    constexpr float NARROW_GAP = 1.5f;
    constexpr float MEDIUM_GAP = 2.5f;
    constexpr float WIDE_GAP   = 3.0f;
    return count > MANY_TICKS ? NARROW_GAP : count > SOME_TICKS ? MEDIUM_GAP : WIDE_GAP;
}

// The ticks go round from the top, a minute each: a focus part's twenty-five
// are fine, a break's five are fat.
void lay_ticks(int count)
{
    count            = std::clamp(count, 1, TICKS_MAX);
    s_tick_count     = count;
    const float step = static_cast<float>(DEGREES_PER_TURN) / static_cast<float>(count);
    const float gap  = tick_gap_degrees(count);
    Face &face = s_full_face;
    if (face.tick[0] == nullptr) {
        return;
    }
    for (int i = 0; i < TICKS_MAX; ++i) {
        lv_obj_set_hidden(face.tick[i], i >= count);
        if (i >= count) {
            continue;
        }
        const auto from = static_cast<int>(DIAL_TOP_DEGREES + step * static_cast<float>(i) + gap / 2.0f);
        const auto to   = static_cast<int>(DIAL_TOP_DEGREES + step * static_cast<float>(i + 1) - gap / 2.0f);
        lv_arc_set_bg_angles(face.tick[i], static_cast<lv_value_precise_t>(from % DEGREES_PER_TURN),
                             static_cast<lv_value_precise_t>(to % DEGREES_PER_TURN));
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
    lv_obj_set_size(fold, LEAF_W, FOLD_H);
    lv_obj_align(fold, LV_ALIGN_LEFT_MID, 0, 0);
    return digits;
}

void build_ticks(Face &face, lv_obj_t *card, std::int32_t ring)
{
    face.ring = clear_box(card);
    lv_obj_set_size(face.ring, ring, ring);
    lv_obj_center(face.ring);
    lv_obj_set_style_radius(face.ring, theme::radius::pill, 0);
    lv_obj_set_adv_hittest(face.ring, true);
    touch_face(face.ring);
    for (lv_obj_t *&tick : face.tick) {
        tick = lv_arc_create(card);
        lv_obj_set_size(tick, ring, ring);
        lv_obj_center(tick);
        lv_arc_set_rotation(tick, 0);
        lv_obj_set_clickable(tick, false);
        lv_obj_set_style_arc_width(tick, TICK_W, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(tick, false, LV_PART_MAIN);
        lv_obj_set_style_arc_color(tick, lv_color_hex(theme::panel), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(tick, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(tick, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(tick, 0, LV_PART_KNOB);
    }
}

// Minutes and seconds each on a leaf, cut across where a flip clock's leaves
// fold, with a colon that beats the seconds between them. Taken as the ring is,
// as the obvious thing to tap.
void build_leaves(Face &face, lv_obj_t *card)
{
    face.leaves = clear_box(card);
    lv_obj_set_size(face.leaves, LV_SIZE_CONTENT, LEAF_H);
    lv_obj_set_flex_flow(face.leaves, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(face.leaves, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(face.leaves, LEAF_GAP, 0);
    lv_obj_center(face.leaves);
    touch_face(face.leaves);
    face.minutes    = leaf(face.leaves);
    lv_obj_t *colon = clear_box(face.leaves);
    lv_obj_set_size(colon, COLON_DOT, COLON_H);
    const lv_align_t places[] = {LV_ALIGN_TOP_MID, LV_ALIGN_BOTTOM_MID};
    for (std::size_t i = 0; i < std::size(face.colon); ++i) {
        face.colon[i] = make_dot(colon, theme::secondary, COLON_DOT);
        lv_obj_align(face.colon[i], places[i], 0, 0);
    }
    face.seconds = leaf(face.leaves);
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

// Until when, or what starting it starts: `start` says how.
void under_text(char *text, std::size_t size, int seconds, const char *start)
{
    const bool idle = s_focus.phase == FocusPhase::Idle;
    if (idle || waiting()) {
        if (s_focus.phase == FocusPhase::Work || idle) {
            std::snprintf(text, size, "%s round %d", start, idle ? 1 : s_focus.round);
        } else {
            std::snprintf(text, size, "%s the break", start);
        }
    } else if (!s_focus.running) {
        std::snprintf(text, size, "Paused, %d min left",
                      static_cast<int>((seconds + units::kSecondsPerMinute - 1) / units::kSecondsPerMinute));
    } else {
        char until[8];
        clock_text(left_ms(), until, sizeof(until));
        std::snprintf(text, size, "Until %s", until);
    }
}

// Once a second while it is up: the leaves, the colon, the ticks, and under
// them until when, or what a tap does.
void show_full_second(bool force)
{
    const std::int32_t left    = left_ms();
    const int          seconds = static_cast<int>(whole_seconds_up(left));
    if (seconds == s_full_shown_s && !force) {
        return;
    }
    s_full_shown_s = seconds;

    const bool         idle    = s_focus.phase == FocusPhase::Idle;
    const bool         paused  = !idle && !s_focus.running;
    const bool         beat    = seconds % 2 == 0;
    const std::int32_t length  = idle ? 0 : s_focus.length_ms;
    const std::int32_t elapsed = length > 0 ? length - left : 0;
    show_leaves(s_full_face, seconds, paused, beat);
    show_ticks(s_full_face, length > 0 ? static_cast<int>(elapsed / units::kMsPerMinute) : 0, idle,
               beat);

    char text[64];
    under_text(text, sizeof(text), seconds, "Tap to start");
    theme::set_text(s_full_under, text);
}

// What the part is, in its colour, and the set's rounds as dots.
void show_full_part()
{
    if (s_full == nullptr) {
        return;
    }
    const bool          idle = s_focus.phase == FocusPhase::Idle;
    const std::uint32_t ink  = ink_of(resting());
    char                text[48];
    std::snprintf(text, sizeof(text), "%s%s", phase_name(s_focus.phase),
                  waiting() ? ", READY" : !idle && !s_focus.running ? ", PAUSED" : "");
    theme::set_text(s_full_phase, text);
    theme::set_text_color(s_full_phase, idle ? theme::secondary : ink);

    const int rounds = std::clamp(s_focus.rounds, 1, ROUNDS_MAX);
    const int done   = idle ? 0 : s_focus.phase == FocusPhase::Work ? s_focus.round - 1 : s_focus.round;
    for (int i = 0; i < ROUNDS_MAX; ++i) {
        lv_obj_set_hidden(s_full_round[i], i >= rounds);
        const bool now = !idle && s_focus.phase == FocusPhase::Work && i == s_focus.round - 1;
        lv_obj_set_style_bg_color(s_full_round[i],
                                  lv_color_hex(i < done || now ? theme::primary : theme::panel_light), 0);
        lv_obj_set_style_bg_opa(s_full_round[i], now && !s_focus.running ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
    show_full_second(true);
}

void open_full()
{
    detail::open_view(s_full_view);
}

// The dial without its card, larger and on the background: the same ticks and
// leaves, the part over them, and under them until when and the set's rounds.
// The leaves pause and carry on, as on the page; the corner chip goes back.
void build_full(lv_obj_t *screen)
{
    const detail::Layout l = detail::layout();
    s_full = lv_obj_create(screen);
    lv_obj_set_size(s_full, l.screen_w, l.screen_h);
    lv_obj_set_pos(s_full, 0, 0);
    theme::style_panel(s_full, theme::background, 0);
    lv_obj_set_scrollable(s_full, false);

    build_ticks(s_full_face, s_full, l.screen_h - 2 * FULL_PAD);
    build_leaves(s_full_face, s_full);

    s_full_phase = theme::make_eyebrow(s_full, "READY");
    lv_obj_align(s_full_phase, LV_ALIGN_CENTER, 0, -LEAF_H / 2 - PHASE_ABOVE_LEAVES);

    s_full_under = theme::make_label(s_full, "", theme::secondary, theme::type_body());
    lv_obj_align(s_full_under, LV_ALIGN_CENTER, 0, LEAF_H / 2 + PHASE_ABOVE_LEAVES);

    lv_obj_t *rounds = clear_box(s_full);
    lv_obj_set_size(rounds, LV_SIZE_CONTENT, FULL_DOT);
    lv_obj_set_flex_flow(rounds, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(rounds, FULL_DOT_GAP, 0);
    lv_obj_align(rounds, LV_ALIGN_CENTER, 0, LEAF_H / 2 + PHASE_ABOVE_LEAVES + FULL_ROUNDS_BELOW);
    for (lv_obj_t *&dot : s_full_round) {
        dot = make_dot(rounds, theme::panel_light, FULL_DOT);
    }
    s_full_view = detail::add_view({"focus", detail::ViewKind::Fullscreen, s_full, show_full_part, nullptr});
    detail::add_fullscreen_chrome(s_full_view, s_full, [](lv_event_t *) { detail::close_view(s_full_view); },
                                  false);  // the timer is the view
    // What the time says around it goes with the buttons, leaving the ring and the time.
    for (lv_obj_t *part : {s_full_phase, s_full_under, rounds}) {
        detail::fade_when_idle(s_full_view, part);
    }
}

// The card from the top row: what the part is, how long is left of it, until
// when and how far it has got, and the controls the fullscreen view has none of.
constexpr std::int32_t POP_W      = 440;
constexpr std::int32_t POP_PAD    = 24;
constexpr std::int32_t POP_INNER  = POP_W - 2 * POP_PAD;
constexpr std::int32_t POP_GAP    = 12;
constexpr std::int32_t POP_BAR_H  = 8;
constexpr std::int32_t POP_BTN_H  = 72;
constexpr std::int32_t POP_SIDE_W = 96;   // reset and skip, beside start
constexpr int          POP_SCALE  = 1000;  // the bar's
constexpr std::uint32_t POP_IDLE_MS = 15 * 1000;
constexpr std::int32_t  FROM_DOCK   = 24;  // the card from the dock's edge, as the desk's

lv_obj_t *s_pop_phase = nullptr;
lv_obj_t *s_pop_time  = nullptr;
lv_obj_t *s_pop_under = nullptr;
lv_obj_t *s_pop_bar   = nullptr;
lv_obj_t *s_pop_go    = nullptr;
lv_obj_t *s_pop_skip  = nullptr;
lv_obj_t *s_pop_reset = nullptr;
int       s_pop_shown_s = -1;

void show_pop_second(bool force)
{
    const std::int32_t left    = left_ms();
    const int          seconds = static_cast<int>(whole_seconds_up(left));
    if (s_pop_time == nullptr || (seconds == s_pop_shown_s && !force)) {
        return;
    }
    s_pop_shown_s = seconds;
    char text[64];
    std::snprintf(text, sizeof(text), "%02d:%02d", seconds / units::kSecondsPerMinute, seconds % units::kSecondsPerMinute);
    theme::set_text(s_pop_time, text);
    under_text(text, sizeof(text), seconds, "Start");
    theme::set_text(s_pop_under, text);
    const bool         idle   = s_focus.phase == FocusPhase::Idle;
    const std::int32_t length = idle ? 0 : s_focus.length_ms;
    lv_bar_set_value(s_pop_bar, length > 0 ? static_cast<std::int32_t>(static_cast<std::int64_t>(POP_SCALE) * (length - left) / length) : 0,
                     LV_ANIM_OFF);
}

void show_pop_part()
{
    if (s_pop_phase == nullptr) {
        return;
    }
    const bool          idle = s_focus.phase == FocusPhase::Idle;
    const std::uint32_t ink  = ink_of(resting());
    char                text[48];
    if (idle) {
        std::snprintf(text, sizeof(text), "READY, %d ROUNDS", s_focus.rounds);
    } else {
        std::snprintf(text, sizeof(text), "%s, ROUND %d OF %d%s", phase_name(s_focus.phase), s_focus.round, s_focus.rounds,
                      !s_focus.running && !waiting() ? ", PAUSED" : "");
    }
    theme::set_text(s_pop_phase, text);
    theme::set_text_color(s_pop_phase, idle ? theme::secondary : ink);
    lv_obj_set_style_bg_color(s_pop_bar, lv_color_hex(ink), LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_pop_time, !idle && !s_focus.running ? LV_OPA_50 : LV_OPA_COVER, 0);

    lv_obj_set_state(s_pop_go, LV_STATE_CHECKED, s_focus.running);
    lv_obj_t *glyph = lv_obj_get_child(s_pop_go, 0);
    theme::set_text(glyph, s_focus.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    theme::set_usable(s_pop_skip, !idle);
    theme::set_usable(s_pop_reset, !idle);
    show_pop_second(true);
}

lv_obj_t *pop_button(lv_obj_t *row, const char *symbol, std::int32_t w, FocusAction action)
{
    lv_obj_t *btn = theme::make_button(row, symbol, theme::panel_light, fonts::size_28());
    lv_obj_set_size(btn, w, POP_BTN_H);
    on_click(btn, action);
    return btn;
}

void build_pop(lv_obj_t *screen)
{
    lv_obj_t *card = detail::build_popout(s_pop, screen, POP_W, detail::GAP, detail::GAP, POP_IDLE_MS);
    lv_obj_set_style_pad_all(card, POP_PAD, 0);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, POP_GAP, 0);

    lv_obj_t *head = clear_box(card);
    lv_obj_set_size(head, POP_INNER, theme::chip::size);
    s_pop_phase = theme::make_eyebrow(head, "READY");
    lv_obj_align(s_pop_phase, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *full = theme::make_chip(head, "");
    theme::make_mark(full, &icons::expand_icon);
    lv_obj_align(full, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(full, lv_color_hex(theme::panel_light), 0);
    lv_obj_add_event_cb(full, [](lv_event_t *) {
        detail::open_popout(s_pop, false);
        open_full();
    }, LV_EVENT_CLICKED, nullptr);

    s_pop_time  = theme::make_label(card, "00:00", theme::text, theme::type_display());
    s_pop_under = theme::make_label(card, "", theme::secondary, theme::type_body());

    s_pop_bar = lv_bar_create(card);
    lv_obj_set_size(s_pop_bar, POP_INNER, POP_BAR_H);
    lv_bar_set_range(s_pop_bar, 0, POP_SCALE);
    theme::style_panel(s_pop_bar, theme::panel_light, POP_BAR_H / 2);
    lv_obj_set_style_radius(s_pop_bar, POP_BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_pop_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_margin_bottom(s_pop_bar, POP_GAP, 0);

    lv_obj_t *row = clear_box(card);
    lv_obj_set_size(row, POP_INNER, POP_BTN_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, POP_GAP, 0);
    s_pop_reset = pop_button(row, LV_SYMBOL_REFRESH, POP_SIDE_W, FocusAction::Reset);
    s_pop_go    = pop_button(row, LV_SYMBOL_PLAY, POP_INNER - 2 * (POP_SIDE_W + POP_GAP), FocusAction::Toggle);
    theme::fill_accent(s_pop_go, LV_STATE_CHECKED);
    s_pop_skip = pop_button(row, LV_SYMBOL_NEXT, POP_SIDE_W, FocusAction::Skip);
    detail::fit_popout(s_pop);
}

// Drawn again as the model changes.
void on_focus()
{
    const Focus &focus    = detail::focus_state();
    const bool   new_part = focus.phase != s_focus.phase || focus.round != s_focus.round ||
                          focus.length_ms != s_focus.length_ms || focus.work_min != s_focus.work_min ||
                          s_tick_count == 0;
    s_focus = focus;
    if (new_part) {
        lay_ticks(focus.phase == FocusPhase::Idle ? focus.work_min
                                                  : std::max<int>(1, focus.length_ms / units::kMsPerMinute));
    }
    show_full_part();
    show_pop_part();
}
}  // namespace

void open_focus_full()
{
    open_full();
}

bool focus_full_open()
{
    return detail::view_open(s_full_view);
}

void toggle_focus_popout(lv_obj_t *button, lv_obj_t *dock, void (*lit)(bool open))
{
    if (!s_pop.open) {
        lv_area_t at;
        lv_area_t side;
        lv_obj_get_coords(button, &at);
        lv_obj_get_coords(dock, &side);
        // Beside the dock, its foot level with the button's at the dock's foot,
        // unfolding up from there.
        const bool right = detail::layout().rail_right;
        s_pop.near       = FROM_DOCK;
        detail::fit_popout(s_pop);
        detail::place_popout(s_pop, right ? side.x1 : side.x2 + 1, at.y2 + 1 + detail::GAP, right, true);
        s_pop.button = button;
        s_pop.above  = dock;
        s_pop.lit    = lit;
        show_pop_part();
    }
    detail::open_popout(s_pop, !s_pop.open);
}

void build_focus_full(lv_obj_t *screen)
{
    build_full(screen);
    build_pop(screen);
    lay_ticks(s_tick_count > 0 ? s_tick_count : s_focus.work_min);
    lv_timer_create(timer_tick, TIMER_PERIOD_MS, nullptr);
    detail::subscribe(detail::Topic::Focus, detail::kNoView, on_focus);
}

}  // namespace ui
