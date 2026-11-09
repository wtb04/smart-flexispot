// The clicker over the whole screen: a laptop's slides on and back, as a
// presenter's clicker does, Desk Link pressing Page Down and Page Up there.
// Next takes most of the screen, so a thumb finds it without looking; under
// them which slide it is, how far through, and how long since the first press
// and on this slide, for a run-through. From the laptop's mark in the bar.

#include "kit_front_views.h"

#include "kit_fonts.h"
#include "status_model.h"
#include "topics.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace ui::kitfront::views {
namespace {
namespace ic = kit::glyph;
using namespace kit;

constexpr std::int32_t EDGE = 32, TOP = 28, CHIP = 64;  // as the chrome's row, which the clock sits in
constexpr std::int32_t BUTTONS_Y = 124, BUTTONS_H = 410, BACK_W = 340, GAP = space::xl;
constexpr std::int32_t RAIL_Y = BUTTONS_Y + BUTTONS_H + space::m, FOOT_Y = RAIL_Y + 44, LINE = 44, NAMES_W = 480;
constexpr std::int32_t RAIL_SPAN = 10000, OVERHANG = 8;  // as the media view's progress
constexpr std::uint32_t TICK_MS = 250;
// After a press its page is shown at once, before the laptop has said; then
// what the laptop says, which knows whether the viewer moved.
constexpr std::uint32_t AHEAD_MS = 1500;
// Held, a button goes on by itself, each step sooner than the one before.
constexpr std::uint32_t HELD_MS = 450, FIRST_STEP_MS = 250, FASTEST_STEP_MS = 90;

detail::ViewId s_view = detail::kNoView;
lv_obj_t      *s_back = nullptr, *s_next = nullptr;
lv_obj_t      *s_slide = nullptr, *s_on_slide = nullptr, *s_clock = nullptr;
lv_obj_t      *s_file = nullptr, *s_machine = nullptr;
lv_obj_t      *s_show = nullptr;
lv_timer_t    *s_held = nullptr;
bool           s_held_next = false;
std::uint32_t  s_step_ms   = 0;
int            s_digits = 0;        // the clock's width, as "8:88", "88:88" or "8:88:88", which it changes only twice
bool           s_presenting = false;  // as the laptop last said, or as tapped where it cannot tell
Rail          *s_rail = nullptr;
lv_timer_t    *s_tick = nullptr;
int            s_page     = 1;
bool           s_running  = false;
std::uint32_t  s_began    = 0;
std::uint32_t  s_slide_began = 0;
std::uint32_t  s_pressed  = 0;
bool           s_pressed_once = false;

int seconds_since(std::uint32_t tick) { return static_cast<int>(lv_tick_elaps(tick) / 1000); }

void turn_to(int page)
{
    if (page != s_page) {
        s_page        = page;
        s_slide_began = lv_tick_get();
    }
}

void paint()
{
    const detail::StatusState &st = detail::status_state();
    const bool reads = st.clicker_pages > 0;
    if (reads && (!s_pressed_once || lv_tick_elaps(s_pressed) > AHEAD_MS)) {
        turn_to(st.clicker_page);
    }

    char buf[96];
    if (reads) {
        std::snprintf(buf, sizeof buf, "%d of %d", s_page, st.clicker_pages);
    } else {
        std::snprintf(buf, sizeof buf, "Slide %d", s_page);
    }
    set_text_if(s_slide, buf);
    if (reads) {
        s_rail->set_value(s_page * RAIL_SPAN / st.clicker_pages);
    }
    lv_obj_set_hidden(s_rail->obj(), !reads);

    const int elapsed = s_running ? seconds_since(s_began) : 0;
    const int digits = elapsed >= 3600 ? 5 : elapsed >= 600 ? 4 : 3;
    if (std::exchange(s_digits, digits) != digits) {
        lv_obj_set_width(s_clock, widest(Role::ReadL, {digits == 5 ? "8:88:88" : digits == 4 ? "88:88" : "8:88"}));
    }
    write_clock(s_clock, elapsed);
    if (s_running) {
        const int at = seconds_since(s_slide_began);
        std::snprintf(buf, sizeof buf, "%d:%02d on this slide", at / 60, at % 60);
        set_text_if(s_on_slide, buf);
    }
    lv_obj_set_hidden(s_on_slide, !s_running);

    // Where the laptop cannot read the viewer, the slide is only the presses counted.
    const bool there = st.clicker[0] != '\0';
    set_text_if(s_file, !there ? "No laptop takes slides" : reads ? st.clicker_deck : "Counting presses");
    set_text_if(s_machine, there ? st.clicker : "");
    set_usable(s_back, there);
    set_usable(s_next, there);

    if (st.clicker_presenting >= 0) {
        s_presenting = st.clicker_presenting == 1;
    }
    lv_obj_set_hidden(s_show, !there || !st.clicker_shows);
    static int s_shown = -1;
    if (std::exchange(s_shown, s_presenting ? 1 : 0) != (s_presenting ? 1 : 0)) {
        set_icon(lv_obj_get_child(s_show, 0), s_presenting ? ic::stop : ic::play_arrow, true);
    }
}

void show_or_leave()
{
    if (detail::s_handlers.slide_show != nullptr) {
        detail::s_handlers.slide_show();
    }
    s_presenting = !s_presenting;  // until the laptop says
    paint();
}

// On the press, not its release, as a clicker's button does.
void press(bool next)
{
    if (detail::s_handlers.slide != nullptr) {
        detail::s_handlers.slide(next);
    }
    const int pages = detail::status_state().clicker_pages;
    const int page  = s_page + (next ? 1 : -1);
    turn_to(pages > 0 ? std::clamp(page, 1, pages) : std::max(1, page));
    s_pressed      = lv_tick_get();
    s_pressed_once = true;
    if (!s_running) {
        s_running     = true;
        s_began       = s_pressed;
        s_slide_began = s_pressed;
        lv_timer_resume(s_tick);
    }
    paint();
}

// A press that has reached the deck's end goes no further, held or not.
bool at_end(bool next)
{
    const int pages = detail::status_state().clicker_pages;
    return pages > 0 && s_page == (next ? pages : 1);
}

void let_go()
{
    if (s_held != nullptr) {
        lv_timer_delete(s_held);
        s_held = nullptr;
    }
}

void held_step(lv_timer_t *t)
{
    if (at_end(s_held_next)) {
        let_go();
        return;
    }
    press(s_held_next);
    s_step_ms = s_step_ms == 0 ? FIRST_STEP_MS : std::max(FASTEST_STEP_MS, s_step_ms * 4 / 5);
    lv_timer_set_period(t, s_step_ms);
}

void restart()
{
    s_running = false;
    if (detail::status_state().clicker_pages == 0) {
        s_page = 1;
    }
    lv_timer_pause(s_tick);
    paint();
}

lv_obj_t *side(lv_obj_t *root, Button kind, const char *glyph, const char *words, std::int32_t x, std::int32_t w,
               bool next)
{
    lv_obj_t *b = button(root, kind, glyph, words);
    lv_obj_set_pos(b, x, BUTTONS_Y);
    lv_obj_set_size(b, w, BUTTONS_H);
    lv_obj_set_style_radius(b, radius::tile, 0);
    lv_obj_set_style_pad_column(b, space::s, 0);
    lv_obj_t *g = lv_obj_get_child(b, 0);
    lv_obj_set_user_data(g, reinterpret_cast<void *>(static_cast<std::intptr_t>(34)));  // the size set_icon keeps
    set_icon(g, glyph, kind == Button::Accent);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 1), fonts::h1(), 0);
    // The kit's press shrinks a button and springs it back, which at this size
    // redraws most of the screen each frame, past what the panel keeps up
    // with: here only its colour, at once both ways, as a clicker clicks.
    lv_obj_set_style_transform_scale_x(b, LV_SCALE_NONE, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(b, LV_SCALE_NONE, LV_STATE_PRESSED);
    static constexpr lv_style_prop_t COLOUR[] = {LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_SHADOW_OPA,
                                                 LV_STYLE_PROP_INV};
    static lv_style_transition_dsc_t at_once;
    lv_style_transition_dsc_init(&at_once, COLOUR, lv_anim_path_linear, 0, 0, nullptr);
    lv_obj_set_style_transition(b, &at_once, LV_STATE_DEFAULT);
    lv_obj_set_style_transition(b, &at_once, LV_STATE_PRESSED);
    lv_obj_add_event_cb(
        b,
        [](lv_event_t *e) {
            s_held_next = lv_event_get_user_data(e) != nullptr;
            if (!at_end(s_held_next)) {
                press(s_held_next);
            }
            let_go();
            s_step_ms = 0;
            s_held    = lv_timer_create(held_step, HELD_MS, nullptr);
        },
        LV_EVENT_PRESSED, next ? reinterpret_cast<void *>(1) : nullptr);
    for (const lv_event_code_t up : {LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(b, [](lv_event_t *) { let_go(); }, up, nullptr);
    }
    return b;
}

void build()
{
    lv_obj_t *root = fullscreen_root();
    s_view         = detail::add_view({"clicker", detail::ViewKind::Fullscreen, root,
                                       [] {
                                           paint();
                                           if (s_running) {
                                               lv_timer_resume(s_tick);
                                           }
                                       },
                                       [] {
                                           lv_timer_pause(s_tick);
                                           let_go();
                                       }});

    s_back = side(root, Button::Raised, ic::chevron_left, "Back", EDGE, BACK_W, false);
    s_next = side(root, Button::Accent, ic::chevron_right, "Next", EDGE + BACK_W + GAP,
                  size::screen_w - 2 * EDGE - BACK_W - GAP, true);
    // the words before the chevron on Next, which points on
    lv_obj_move_to_index(lv_obj_get_child(s_next, 0), -1);

    RailSpec spec;
    spec.length = size::screen_w - 2 * EDGE + 2 * OVERHANG;
    spec.max    = RAIL_SPAN;
    spec.quiet  = true;
    s_rail      = new Rail(root, spec);
    lv_obj_set_pos(s_rail->obj(), EDGE - OVERHANG, RAIL_Y);
    lv_obj_set_clickable(s_rail->obj(), false);

    s_slide = text(root, Role::H2, "");
    lv_obj_set_width(s_slide, widest(Role::H2, {"Slide 888", "888 of 888"}));
    lv_obj_set_pos(s_slide, EDGE, FOOT_Y);
    s_on_slide = text(root, Role::Small, "", Ink::Secondary);
    lv_obj_set_width(s_on_slide, widest(Role::Small, {"88:88 on this slide"}));
    lv_obj_set_pos(s_on_slide, EDGE, FOOT_Y + LINE);

    for (lv_obj_t **name : {&s_file, &s_machine}) {
        const bool file = name == &s_file;
        *name = text(root, file ? Role::H3 : Role::Small, "", file ? Ink::Primary : Ink::Secondary);
        lv_obj_set_width(*name, NAMES_W);
        lv_label_set_long_mode(*name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(*name, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(*name, LV_ALIGN_TOP_RIGHT, -EDGE, file ? FOOT_Y + space::xs : FOOT_Y + LINE);
    }

    // In the chrome's row, between its chips: the run-through's clock and its restart.
    lv_obj_t *top = flow(root, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(top, CHIP);
    s_clock = text(top, Role::ReadL, "");
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *again = fill_button(top, ic::restart_alt, CHIP, 30);  // as the chips either side
    lv_obj_add_event_cb(again, [](lv_event_t *) { restart(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, TOP);

    // Tapped all through a run-through, the chips would only come back each
    // time: they stay. Beside the way to the desk, the viewer's full screen:
    // play to present, stop to leave.
    const Chrome over = chrome(s_view, root, false, false);
    s_show = quick_action(over, ic::play_arrow);
    lv_obj_add_event_cb(s_show, [](lv_event_t *) { show_or_leave(); }, LV_EVENT_CLICKED, nullptr);
    s_tick = lv_timer_create([](lv_timer_t *) { paint(); }, TICK_MS, nullptr);
    lv_timer_pause(s_tick);
    detail::subscribe(Topic::Status, s_view, paint);
}
}  // namespace

void open_clicker()
{
    close_popover(false);
    detail::open_view(s_view);
}

void build_clicker(lv_obj_t *) { build(); }
}  // namespace ui::kitfront::views
