// The focus timer over the whole screen, calm: how long is left, large, in a
// thick ring that runs down round it, what part it is and until when; a tap
// starts or pauses it, a swipe to the left goes on to the next part. And the
// card from the dock's button, the same ring smaller, with what the view has
// none of: reset, start, skip.

#include "kit_front_views.h"

#include "focus_model.h"
#include "kit_fonts.h"
#include "topics.h"

#include <cstdio>
#include <ctime>
#include <utility>

namespace ui::kitfront::views {
namespace {
namespace ic = kit::glyph;
using namespace kit;
using detail::focus_state;

constexpr std::uint32_t TICK_MS = 250;
constexpr std::int32_t  RING_SPAN = 3600;  // the ring's steps: a tenth of a degree, finer than it draws

lv_timer_t *s_tick = nullptr;

void act(FocusAction action)
{
    if (detail::s_handlers.focus != nullptr) {
        detail::s_handlers.focus(action);
    }
}

const char *phase_name(FocusPhase phase)
{
    switch (phase) {
        case FocusPhase::Work:      return "Focus";
        case FocusPhase::Break:     return "Break";
        case FocusPhase::LongBreak: return "Long break";
        case FocusPhase::Idle:      break;
    }
    return "Ready";
}

Ink phase_ink(const Focus &f)
{
    return detail::focus_idle(f) ? Ink::Secondary : detail::focus_resting(f) ? Ink::Green : Ink::Accent;
}

void phase_text(const Focus &f, char *out, std::size_t size)
{
    if (detail::focus_idle(f)) {
        std::snprintf(out, size, "Ready, %d rounds", f.rounds);
        return;
    }
    std::snprintf(out, size, "%s, round %d of %d%s", phase_name(f.phase), f.round, f.rounds,
                  detail::focus_paused(f) && !detail::focus_waiting(f) ? ", paused" : "");
}

// Until when, or what starting it starts: `start` says how.
void under_text(const Focus &f, int seconds, const char *start, char *out, std::size_t size)
{
    const bool idle = detail::focus_idle(f);
    if (idle || detail::focus_waiting(f)) {
        if (idle || f.phase == FocusPhase::Work) {
            std::snprintf(out, size, "%s round %d", start, idle ? 1 : f.round);
        } else {
            std::snprintf(out, size, "%s the break", start);
        }
    } else if (!f.running) {
        std::snprintf(out, size, "Paused, %d min left", (seconds + 59) / 60);
    } else {
        const std::time_t at = detail::wall_now() + seconds;
        std::tm           local{};
        localtime_r(&at, &local);
        std::snprintf(out, size, "Until %02d:%02d", local.tm_hour, local.tm_min);
    }
}

void time_text(int seconds, char *out, std::size_t size)
{
    std::snprintf(out, size, "%02d:%02d", seconds / 60, seconds % 60);
}

std::int32_t left_steps(const Focus &f, std::int32_t left)
{
    const std::int32_t length = detail::focus_idle(f) ? 0 : f.length_ms;
    return length > 0 ? static_cast<std::int32_t>(static_cast<std::int64_t>(left) * RING_SPAN / length) : RING_SPAN;
}

// The ring runs down as the dock's time does: green resting, the accent
// working, faded while paused or not yet started. It steps once a second
// without a spring, and draws again only the degree it gave up.
Dial *ring(lv_obj_t *parent, std::int32_t size, std::int32_t width, Role read, bool over)
{
    DialSpec spec;
    spec.size    = size;
    spec.arc     = width;
    spec.start   = 270;
    spec.sweep   = 360;
    spec.max     = RING_SPAN;
    spec.reverse = true;
    spec.read    = read;
    spec.unit    = "";
    spec.over    = over;
    auto *dial   = new Dial(parent, spec);
    lv_obj_add_event_cb(dial->obj(), [](lv_event_t *e) { delete static_cast<Dial *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, dial);
    lv_obj_set_width(dial->figure(), widest(read, {"00:00"}));
    return dial;
}

void paint_ring(Dial *dial, const Focus &f)
{
    dial->set_ink(detail::focus_resting(f) ? Ink::Green : Ink::Accent);
    dial->set_faded(detail::focus_paused(f) || detail::focus_idle(f));
}

// --- the fullscreen timer ---------------------------------------------------

namespace full {
constexpr std::int32_t RING = 600, RING_W = 32, TEXT_W = 420;

detail::ViewId s_view = detail::kNoView;
lv_obj_t      *s_phase = nullptr, *s_time = nullptr, *s_under = nullptr;
Dial          *s_ring = nullptr;
int            s_shown_s = -1;
bool           s_swiped  = false;  // the release that ends a swipe is not a tap

void show_second(bool force)
{
    const Focus       &f       = focus_state();
    const std::int32_t left    = detail::focus_left_ms(f);
    const int          seconds = static_cast<int>(detail::whole_seconds_up(left));
    if (std::exchange(s_shown_s, seconds) == seconds && !force) {
        return;
    }
    char buf[64];
    time_text(seconds, buf, sizeof buf);
    set_text_if(s_time, buf);
    under_text(f, seconds, "Tap to start", buf, sizeof buf);
    set_text_if(s_under, buf);
    s_ring->set_value(left_steps(f, left));
}

void show_part()
{
    const Focus &f = focus_state();
    char         buf[48];
    phase_text(f, buf, sizeof buf);
    set_text_if(s_phase, buf);
    set_ink(s_phase, phase_ink(f));
    set_ink(s_time, detail::focus_paused(f) ? Ink::Secondary : Ink::Primary);
    paint_ring(s_ring, f);
    show_second(true);
}

void touched(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        s_swiped = true;
        if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_LEFT) {
            act(FocusAction::Skip);
        }
        return;
    }
    if (!std::exchange(s_swiped, false)) {
        act(FocusAction::Toggle);
    }
}

void build()
{
    lv_obj_t *root = fullscreen_root();
    s_view         = detail::add_view({"focus", detail::ViewKind::Fullscreen, root,
                                       [] {
                                           s_shown_s = -1;
                                           lv_timer_resume(s_tick);
                                       },
                                       nullptr});
    lv_obj_add_event_cb(root, touched, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(root, touched, LV_EVENT_GESTURE, nullptr);

    s_ring = ring(root, RING, RING_W, Role::ReadXXL, true);
    lv_obj_center(s_ring->obj());
    s_time  = s_ring->figure();
    s_under = s_ring->caption();
    s_phase = s_ring->over();
    lv_obj_set_style_text_font(s_phase, fonts::h3(), 0);
    lv_obj_set_style_text_font(s_under, fonts::body(), 0);
    for (lv_obj_t *label : {s_phase, s_under}) {
        lv_obj_set_width(label, TEXT_W);
    }
    lv_obj_set_style_margin_bottom(s_phase, space::l, 0);
    lv_obj_set_style_margin_top(s_under, space::s, 0);
    for (lv_obj_t *label : {s_time, s_phase, s_under}) {
        lv_obj_set_clickable(label, false);
    }

    chrome(s_view, root, false);  // the timer is the view: no badge of it over it
    // what is said round the time goes with the buttons, leaving the time and how far
    detail::fade_when_idle(s_view, s_phase);
    detail::fade_when_idle(s_view, s_under);
    detail::subscribe(Topic::Focus, s_view, show_part);
}
}  // namespace full

// --- the card from the dock -------------------------------------------------

namespace card {
constexpr std::int32_t W = 440, PAD = space::xxl, INNER = W - 2 * PAD, GAP = space::m, SIDE_W = 80;
constexpr std::int32_t RING = 300, RING_W = 24;

lv_obj_t *s_body = nullptr, *s_phase = nullptr, *s_time = nullptr, *s_under = nullptr;
lv_obj_t *s_go = nullptr, *s_reset = nullptr, *s_skip = nullptr;
Dial     *s_ring = nullptr;
int       s_shown_s = -1;

void show_second(bool force)
{
    const Focus       &f       = focus_state();
    const std::int32_t left    = detail::focus_left_ms(f);
    const int          seconds = static_cast<int>(detail::whole_seconds_up(left));
    if (std::exchange(s_shown_s, seconds) == seconds && !force) {
        return;
    }
    char buf[64];
    time_text(seconds, buf, sizeof buf);
    set_text_if(s_time, buf);
    under_text(f, seconds, "Start", buf, sizeof buf);
    set_text_if(s_under, buf);
    s_ring->set_value(left_steps(f, left));
}

void show_part()
{
    if (s_body == nullptr) {
        return;
    }
    const Focus &f    = focus_state();
    const bool   idle = detail::focus_idle(f);
    char         buf[48];
    phase_text(f, buf, sizeof buf);
    set_text_if(s_phase, buf);
    set_ink(s_phase, phase_ink(f));
    set_ink(s_time, detail::focus_paused(f) ? Ink::Secondary : Ink::Primary);
    set_icon(lv_obj_get_child(s_go, 0), f.running ? ic::pause : ic::play_arrow, true);
    set_usable(s_reset, !idle);
    set_usable(s_skip, !idle);
    paint_ring(s_ring, f);
    show_second(true);
}

lv_obj_t *control(lv_obj_t *row, Button kind, const char *glyph, std::int32_t w, FocusAction action)
{
    lv_obj_t *b = button(row, kind, glyph, nullptr);
    lv_obj_set_width(b, w);
    lv_obj_add_event_cb(
        b, [](lv_event_t *e) { act(static_cast<FocusAction>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)))); },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(action)));
    return b;
}

void open(lv_obj_t *source)
{
    const std::int32_t h = 2 * PAD + size::button + RING + size::button + 2 * GAP;
    s_body = popover(source, W, h, From::Beside);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_body, GAP, 0);
    lv_obj_add_event_cb(
        s_body,
        [](lv_event_t *) {
            s_ring = nullptr;
            s_body = nullptr;
        },
        LV_EVENT_DELETE, nullptr);

    lv_obj_t *head = flow(s_body, LV_FLEX_FLOW_ROW, GAP);
    lv_obj_set_size(head, INNER, size::button);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_phase = text(head, Role::Small, "", Ink::Secondary);
    lv_obj_set_flex_grow(s_phase, 1);
    lv_label_set_long_mode(s_phase, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *full = button(head, Button::Flat, ic::open_in_full, nullptr, true);
    lv_obj_set_size(full, size::button, size::button);
    lv_obj_set_style_margin_right(full, -space::s, 0);
    lv_obj_add_event_cb(full, [](lv_event_t *) { open_focus_view(); }, LV_EVENT_CLICKED, nullptr);

    s_ring = ring(s_body, RING, RING_W, Role::ReadL, false);
    s_time  = s_ring->figure();
    s_under = s_ring->caption();
    lv_obj_set_width(s_under, RING - 2 * RING_W - 2 * space::l);

    lv_obj_t *row = flow(s_body, LV_FLEX_FLOW_ROW, GAP);
    lv_obj_set_size(row, INNER, size::button);
    s_reset = control(row, Button::Raised, ic::restart_alt, SIDE_W, FocusAction::Reset);
    s_go    = control(row, Button::Accent, ic::play_arrow, INNER - 2 * (SIDE_W + GAP), FocusAction::Toggle);
    s_skip  = control(row, Button::Raised, ic::skip_next, SIDE_W, FocusAction::Skip);

    show_part();
    lv_timer_resume(s_tick);
}
}  // namespace card

void tick(lv_timer_t *t)
{
    const bool full_open = detail::view_open(full::s_view);
    if (full_open) {
        full::show_second(false);
    }
    if (card::s_body != nullptr) {
        card::show_second(false);
    }
    if (!full_open && card::s_body == nullptr) {
        lv_timer_pause(t);
    }
}
}  // namespace

void open_focus_view()
{
    close_popover(false);
    detail::open_view(full::s_view);
}

void build_focus(lv_obj_t *)
{
    s_tick = lv_timer_create(tick, TICK_MS, nullptr);
    lv_timer_pause(s_tick);
    full::build();
    detail::subscribe(Topic::Focus, detail::kNoView, card::show_part);
}
}  // namespace ui::kitfront::views

namespace ui::kitfront {
void open_focus(lv_obj_t *source) { views::card::open(source); }
}  // namespace ui::kitfront
