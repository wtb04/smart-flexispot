// The screen as it has been: the dock, the control bar, the pages and the
// fullscreen views drawn with theme.h, built on the shared core in ui.cpp.

#include "ui_internal.h"
#include "focus_view.h"
#include "room_model.h"
#include "status_model.h"
#include "topics.h"

#ifndef SHOT_DESK_CARD
#define SHOT_DESK_CARD 0
#endif
#ifndef SHOT_ENABLED
#define SHOT_ENABLED 0
#endif

namespace ui::detail {
namespace {
constexpr std::uint32_t SHOT_START_MS = 25000;
constexpr std::uint32_t SHOT_PAGE_MS  = 2000;
}  // namespace

Layout layout()
{
    lv_display_t      *disp = lv_display_get_default();
    const std::int32_t w    = lv_display_get_horizontal_resolution(disp);
    const std::int32_t h    = lv_display_get_vertical_resolution(disp);
    return Layout{w,
                  h,
                  s_dock_right ? GAP : GAP + DOCK_W + GAP,
                  CONTENT_Y,
                  w - DOCK_W - 3 * GAP,
                  h - CONTENT_Y - GAP,
                  s_dock_right ? w - GAP - DOCK_W : GAP,
                  s_dock_right};
}

namespace {
// The two move buttons and a button per preset, stand and sit among them.
constexpr int MOVE_BUTTON_COUNT = 2;
constexpr int DESK_CONTROL_MAX  = MOVE_BUTTON_COUNT + kPresetCount + kDeskShortcutButtons;

lv_obj_t     *s_desk_controls[DESK_CONTROL_MAX] = {};
int           s_desk_control_count              = 0;
}  // namespace


void screen_off_cb(lv_event_t *)
{
    set_screen_state(false);
}

// Faded while the desk does not answer.
void register_desk_control(lv_obj_t *obj)
{
    if (s_desk_control_count == 0) {
        subscribe(Topic::Desk, kNoView, [] {
            for (int i = 0; i < s_desk_control_count; ++i) {
                theme::set_usable(s_desk_controls[i], desk_state().available);
            }
        });
    }
    if (s_desk_control_count < DESK_CONTROL_MAX) {
        s_desk_controls[s_desk_control_count++] = obj;
        theme::set_usable(obj, desk_state().available);
    }
}

lv_obj_t *s_dock          = nullptr;  // the dock
lv_obj_t *s_content       = nullptr;

void place_for_side()
{
    const Layout l = layout();
    lv_obj_set_x(s_dock, l.dock_x);
    lv_obj_set_pos(s_content, l.content_x, l.content_y);
    place_desk_card();
    place_top_bar();
    place_notice();
}
namespace {
// One page per tick rather than all at once: a tab's colour eases in, and a
// picture taken straight after the switch shows the old tab lit. Pages hidden
// while the phone is away are the ones most often worth looking at, so the gate
// is lifted for as long as the pictures take.
void take_screenshots(lv_timer_t *timer)
{
    // Only the pages being worked on: every one adds about a minute.
    static const int PAGES[] = {CALENDAR_PAGE};
    static int       step    = -1;
    static bool      gated   = false;
    if (step >= 0) {
        screenshot();
    } else {
        gated           = s_presence_gate;
        s_presence_gate = false;
    }
    if (++step < static_cast<int>(std::size(PAGES))) {
        select_page(PAGES[step]);
        if (SHOT_DESK_CARD) {
            open_desk_card(true);
        }
        lv_timer_set_period(timer, SHOT_PAGE_MS);
        return;
    }
    s_presence_gate = gated;
    select_page(HOME_PAGE);
    lv_timer_delete(timer);
}

void prepare_screen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(theme::background), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

}

// Behind the splash, one part at a time, laid out before the next, so that
// between parts the splash moves on. Each is hidden until the splash leaves.
bool next_part()
{
    static int next = 0;
    lv_obj_t  *scr  = lv_screen_active();
    switch (next++) {
        case 0:
            create_dock(scr);
            create_content(scr);
            create_top_bar(scr);
            return true;
        case 1:
            if (build_next_page()) {
                --next;
            }
            return true;
        case 2:
            // Development: hands a picture of the screen to tools/screenshot.py a
            // little after boot. It holds the LVGL lock for several seconds, so
            // it is off unless wanted.
            if (SHOT_ENABLED) {
                lv_timer_create(take_screenshots, SHOT_START_MS, nullptr);
            }
            create_desk_card(scr);
            build_cinema(scr);
            build_music(scr);
            build_favourites(scr);
            build_focus_full(scr);
            create_notice_card();
            start_screen_schedule();
            follow_pages();
            return true;
        default:
            return false;
    }
}
}  // namespace

}  // namespace ui::detail

namespace ui::front {
void init(std::uint32_t accent)
{
    fonts::init();
    theme::init_accents();
    if (accent != 0) {
        theme::set_primary(accent);
    }
    detail::prepare_screen();
    detail::build_splash();
}

bool build_next_part()
{
    const bool more = detail::next_part();
    detail::keep_under_splash();
    return more;
}

void apply() { detail::apply_splash(); }
}  // namespace ui::front
