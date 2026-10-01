#include "ui_internal.h"

#include <ctime>
#include <vector>

// The time in a fullscreen view's bottom right corner, shown or put away by a
// tap there: the same in each view, as the last tap left it. The hours and
// minutes in the large digits, the dots between them drawn, the day under.
namespace ui::detail {
namespace {
constexpr std::int32_t  CORNER_W   = 320;  // where a tap shows it or puts it away
constexpr std::int32_t  CORNER_H   = 170;
constexpr std::int32_t  INSET      = 40;
constexpr std::int32_t  DOT        = 9;
constexpr std::int32_t  DOT_GAP    = 14;   // between the two dots
constexpr std::int32_t  COLON_PAD  = 10;   // either side of them
constexpr std::int32_t  DATE_GAP   = 2;
constexpr std::uint32_t FADE_MS    = 200;
constexpr std::uint32_t TICK_MS    = 1000;
constexpr time_t        CLOCK_SET  = 1'700'000'000;  // any earlier and the clock is not set yet

struct Clock {
    lv_obj_t *face    = nullptr;  // what fades
    lv_obj_t *hours   = nullptr;
    lv_obj_t *minutes = nullptr;
    lv_obj_t *date    = nullptr;
};
std::vector<Clock> s_clocks;
bool               s_shown = false;
lv_timer_t        *s_tick  = nullptr;

void set_face_opa(void *face, std::int32_t opa)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(face), static_cast<lv_opa_t>(opa), 0);
}

void fade(lv_obj_t *face, bool in)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, face);
    lv_anim_set_exec_cb(&anim, set_face_opa);
    lv_anim_set_values(&anim, lv_obj_get_style_opa(face, LV_PART_MAIN), in ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, FADE_MS);
    if (in) {
        lv_obj_set_hidden(face, false);
    } else {
        lv_anim_set_completed_cb(&anim, [](lv_anim_t *done) {
            lv_obj_set_hidden(static_cast<lv_obj_t *>(done->var), true);
        });
    }
    lv_anim_start(&anim);
}

void tell_time(lv_timer_t *)
{
    const time_t now = std::time(nullptr);
    std::tm      local{};
    localtime_r(&now, &local);
    const bool set = now >= CLOCK_SET;
    char hours[4], minutes[4], date[40];
    std::snprintf(hours, sizeof(hours), "%02d", local.tm_hour);
    std::snprintf(minutes, sizeof(minutes), "%02d", local.tm_min);
    std::strftime(date, sizeof(date), "%A %-d %B", &local);
    for (const Clock &clock : s_clocks) {
        if (lv_obj_is_hidden(clock.face)) {
            continue;
        }
        theme::set_text(clock.hours, set ? hours : "--");
        theme::set_text(clock.minutes, set ? minutes : "--");
        theme::set_text(clock.date, set ? date : "");
    }
}

void corner_tapped(lv_event_t *)
{
    s_shown = !s_shown;
    for (const Clock &clock : s_clocks) {
        fade(clock.face, s_shown);
    }
    tell_time(nullptr);
}

lv_obj_t *dot(lv_obj_t *parent)
{
    lv_obj_t *d = lv_obj_create(parent);
    theme::style_panel(d, theme::secondary, theme::radius::pill);
    lv_obj_set_size(d, DOT, DOT);
    lv_obj_set_clickable(d, false);
    return d;
}
}  // namespace

void add_corner_clock(lv_obj_t *root)
{
    const Layout l = layout();
    lv_obj_t    *corner = lv_obj_create(root);
    lv_obj_set_size(corner, CORNER_W, CORNER_H);
    lv_obj_set_pos(corner, l.screen_w - CORNER_W, l.screen_h - CORNER_H);
    lv_obj_set_style_bg_opa(corner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(corner, 0, 0);
    lv_obj_set_style_pad_all(corner, 0, 0);
    lv_obj_set_scrollable(corner, false);
    lv_obj_add_event_cb(corner, corner_tapped, LV_EVENT_CLICKED, nullptr);
    lv_obj_move_to_index(corner, 0);  // under the view's own controls, where they reach into it

    Clock clock;
    clock.face = lv_obj_create(corner);
    lv_obj_set_size(clock.face, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(clock.face, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clock.face, 0, 0);
    lv_obj_set_style_pad_all(clock.face, 0, 0);
    lv_obj_set_flex_flow(clock.face, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clock.face, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(clock.face, DATE_GAP, 0);
    lv_obj_set_clickable(clock.face, false);
    lv_obj_align(clock.face, LV_ALIGN_BOTTOM_RIGHT, -INSET, -INSET);

    lv_obj_t *time = lv_obj_create(clock.face);
    lv_obj_set_size(time, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time, 0, 0);
    lv_obj_set_style_pad_all(time, 0, 0);
    lv_obj_set_flex_flow(time, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(time, false);
    clock.hours = theme::make_label(time, "--", theme::text, fonts::temp_64());
    lv_obj_t *colon = lv_obj_create(time);
    lv_obj_set_size(colon, DOT + 2 * COLON_PAD, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(colon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(colon, 0, 0);
    lv_obj_set_style_pad_all(colon, 0, 0);
    lv_obj_set_flex_flow(colon, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(colon, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(colon, DOT_GAP, 0);
    lv_obj_set_clickable(colon, false);
    dot(colon);
    dot(colon);
    clock.minutes = theme::make_label(time, "--", theme::text, fonts::temp_64());
    clock.date    = theme::make_label(clock.face, "", theme::secondary, fonts::size_22());

    lv_obj_set_hidden(clock.face, !s_shown);
    lv_obj_set_style_opa(clock.face, s_shown ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    s_clocks.push_back(clock);
    if (s_tick == nullptr) {
        s_tick = lv_timer_create(tell_time, TICK_MS, nullptr);
    }
}
}  // namespace ui::detail
