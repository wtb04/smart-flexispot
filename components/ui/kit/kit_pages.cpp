#include "kit.h"

#include "kit_bake.h"
#include "kit_fonts.h"
#include "lvgl_private.h"  // invalidating an area of the display, and how far an object draws past itself

#if defined(ESP_PLATFORM)
#include "board.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>

namespace ui::kit {
namespace {

constexpr int          MAX_PAGES = 8;
constexpr int          BANDS     = 8;   // about a ninth of the screen a frame, and eight frames still read as one sweep
constexpr int          MAX_UNITS = 24;
constexpr std::int32_t REACH     = 32;  // further than any tile's shadow falls
constexpr std::int32_t PROGRESS  = 1024;

bool meet(lv_area_t *out, const lv_area_t &a, const lv_area_t &b)
{
    *out = {std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

lv_area_t span(const lv_area_t &a, const lv_area_t &b)
{
    if (a.x1 > a.x2) {
        return b;
    }
    if (b.x1 > b.x2) {
        return a;
    }
    return {std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2), std::max(a.y2, b.y2)};
}

constexpr lv_area_t NOTHING{1, 1, 0, 0};
// Where anything of a page can be: all but the dock's side.
constexpr lv_area_t PAGES{inset::left - space::l, 0, size::screen_w - 1, size::screen_h - 1};

struct Page {
    lv_obj_t *obj   = nullptr;
    lv_obj_t *item  = nullptr;
    void (*build)(lv_obj_t *) = nullptr;
    bool      lit   = false;  // its item has the mark behind it
};
Page   s_pages[MAX_PAGES];
int    s_page_count = 0;
int    s_shown      = -1;
Change s_how        = Change::Slide;
void (*s_told)(int) = nullptr;
lv_obj_t *s_mark    = nullptr;
lv_obj_t *s_grids[MAX_PAGES]{};
int       s_grid_count = 0;
std::uint32_t s_frames = 0;

// One change from a page to the next. What is not open yet is not drawn: the
// screen keeps the old page there until its band comes.
struct Run {
    bool         active = false, filtering = false, nested = false;
    Change       how    = Change::Instant;
    int          from = -1, to = -1;
    lv_area_t    region{}, open{};
    int          step = 0, steps = 0;
    std::int32_t centre = 0;
    lv_obj_t    *units[MAX_UNITS]{};
    int          unit_count = 0;
    lv_timer_t  *stepper  = nullptr;
    std::uint32_t frame_at = 0, ms_at = 0;
    std::uint64_t began_us = 0, ready_us = 0, first_us = 0, bake_us = 0, told_us = 0;
    std::uint32_t frames = 0;
} s_run;

char          s_report[240] = "no page change yet";
std::uint32_t s_run_id      = 1;

const char *name_of(Change how)
{
    switch (how) {
    case Change::Instant: return "instant";
    case Change::Wipe: return "wipe";
    case Change::Spread: return "spread";
    case Change::Stagger: return "stagger";
    case Change::Slide: return "slide";
    case Change::Fade: return "fade";
    }
    return "";
}

bool banded(Change how) { return how == Change::Wipe || how == Change::Spread; }
bool whole(Change how) { return how == Change::Slide || how == Change::Fade; }

// LVGL cannot be told no: an area that is all in what is not open yet becomes
// the screen's corner, which is the ground and is drawn the same again.
// While a page slides on the panel only what is round it is drawn, into each
// step's frame: inside, the copy is what shows, and what changed there is
// drawn once the slide is over.
bool      s_gliding = false;
lv_area_t s_glide_region{}, s_glide_dropped = NOTHING;

bool clip_to_glide(lv_event_t *e)
{
    auto           *a = static_cast<lv_area_t *>(lv_event_get_param(e));
    const lv_area_t r = s_glide_region;
    lv_area_t       in;
    if (!meet(&in, *a, r)) {
        return true;
    }
    s_glide_dropped = s_glide_dropped.x1 > s_glide_dropped.x2 ? in : span(s_glide_dropped, in);
    lv_area_t pieces[4];
    int       n = 0;
    if (a->y1 < r.y1) {
        pieces[n++] = {a->x1, a->y1, a->x2, r.y1 - 1};
    }
    if (a->y2 > r.y2) {
        pieces[n++] = {a->x1, r.y2 + 1, a->x2, a->y2};
    }
    if (a->x1 < r.x1) {
        pieces[n++] = {a->x1, in.y1, r.x1 - 1, in.y2};
    }
    if (a->x2 > r.x2) {
        pieces[n++] = {r.x2 + 1, in.y1, a->x2, in.y2};
    }
    if (n == 0) {
        *a = {0, 0, 0, 0};  // the screen's corner, under the dock, drawn the same again
        return true;
    }
    s_run.nested = true;
    for (int i = 1; i < n; ++i) {
        lv_inv_area(static_cast<lv_display_t *>(lv_event_get_current_target(e)), &pieces[i]);
    }
    s_run.nested = false;
    *a = pieces[0];
    return true;
}

void on_invalidate(lv_event_t *e)
{
    if (s_gliding && !s_run.nested) {
        clip_to_glide(e);
        return;
    }
    if (!s_run.filtering || s_run.nested) {
        return;
    }
    auto           *a = static_cast<lv_area_t *>(lv_event_get_param(e));
    const lv_area_t r = s_run.region;
    lv_area_t       in;
    if (!meet(&in, *a, r)) {
        return;
    }
    lv_area_t pieces[5];
    int       n = 0;
    if (a->y1 < r.y1) {
        pieces[n++] = {a->x1, a->y1, a->x2, r.y1 - 1};
    }
    if (a->y2 > r.y2) {
        pieces[n++] = {a->x1, r.y2 + 1, a->x2, a->y2};
    }
    if (a->x1 < r.x1) {
        pieces[n++] = {a->x1, in.y1, r.x1 - 1, in.y2};
    }
    if (a->x2 > r.x2) {
        pieces[n++] = {r.x2 + 1, in.y1, a->x2, in.y2};
    }
    lv_area_t live;
    if (meet(&live, in, s_run.open)) {
        pieces[n++] = live;
    }
    if (n == 0) {
        *a = {0, 0, 0, 0};
        return;
    }
    s_run.nested = true;
    for (int i = 1; i < n; ++i) {
        lv_inv_area(static_cast<lv_display_t *>(lv_event_get_current_target(e)), &pieces[i]);
    }
    s_run.nested = false;
    *a = pieces[0];
}

void on_frame(lv_event_t *)
{
    ++s_frames;
    if (s_run.active) {
        ++s_run.frames;
        if (s_run.first_us == 0) {
            s_run.first_us = perf::now_us();
        }
    }
}

void hooks()
{
    static bool hooked = false;
    if (hooked) {
        return;
    }
    hooked = true;
    lv_display_t *disp = lv_display_get_default();
    lv_display_add_event_cb(disp, on_invalidate, LV_EVENT_INVALIDATE_AREA, nullptr);
    lv_display_add_event_cb(disp, on_frame, LV_EVENT_REFR_READY, nullptr);
    // the pages not yet made, one in each pause, so a first visit finds its page ready
    lv_timer_create(
        [](lv_timer_t *) {
            if (s_run.active || perf::now_us() - perf::last_frame_us() < 300000) {
                return;
            }
            for (Page &p : s_pages) {
                if (p.build != nullptr) {
                    auto fill = p.build;
                    p.build   = nullptr;
                    fill(p.obj);
                    lv_obj_update_layout(p.obj);
                    return;
                }
            }
        },
        100, nullptr);
}

void build(Page &p)
{
    if (p.build != nullptr) {
        auto b  = p.build;
        p.build = nullptr;
        b(p.obj);
    }
}

// What of the page is drawn, as it is scrolled now: everything on it, with
// room for the shadows, cut to the page.
void grow(lv_area_t *box, lv_obj_t *obj)
{
    const std::uint32_t n = lv_obj_get_child_count(obj);
    for (std::uint32_t i = 0; i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(obj, static_cast<std::int32_t>(i));
        if (lv_obj_is_hidden(c)) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        const std::int32_t ext = std::max(REACH, lv_obj_get_ext_draw_size(c));
        lv_area_increase(&a, ext, ext);
        *box = span(*box, a);
        if (lv_obj_is_overflow_visible(c)) {
            grow(box, c);
        }
    }
}

lv_area_t drawn(lv_obj_t *page)
{
    lv_area_t box = NOTHING;
    grow(&box, page);
    lv_area_t out;
    return meet(&out, box, PAGES) ? out : NOTHING;
}

void quiet(bool on)
{
    lv_display_enable_invalidation(lv_display_get_default(), !on);
}

void open_to(const lv_area_t &now)
{
    // what is newly open: the region's bands either side of what was open
    const lv_area_t was = s_run.open;
    s_run.open          = now;
    if (was.x1 > was.x2) {
        lv_obj_invalidate_area(lv_screen_active(), &now);
        return;
    }
    const lv_area_t parts[] = {{now.x1, now.y1, now.x2, was.y1 - 1},
                               {now.x1, was.y2 + 1, now.x2, now.y2},
                               {now.x1, was.y1, was.x1 - 1, was.y2},
                               {was.x2 + 1, was.y1, now.x2, was.y2}};
    for (const lv_area_t &p : parts) {
        lv_area_t a;
        if (meet(&a, p, now)) {
            lv_obj_invalidate_area(lv_screen_active(), &a);
        }
    }
}

std::int32_t item_centre(int index)
{
    lv_obj_t *item = s_pages[index].item;
    if (item == nullptr) {
        return size::screen_h / 2;
    }
    lv_area_t a;
    lv_obj_get_coords(item, &a);
    return (a.y1 + a.y2) / 2;
}

void step()
{
    const lv_area_t &r = s_run.region;
    const int        k = ++s_run.step;
    if (s_run.how == Change::Wipe) {
        const std::int32_t x = r.x1 + (r.x2 - r.x1 + 1) * k / s_run.steps;
        open_to({r.x1, r.y1, x - 1, r.y2});
    } else if (s_run.how == Change::Spread) {
        const std::int32_t reach = std::max(s_run.centre - r.y1, r.y2 - s_run.centre) + 1;
        const std::int32_t d     = reach * k / s_run.steps;
        open_to({r.x1, std::max(r.y1, s_run.centre - d), r.x2, std::min(r.y2, s_run.centre + d)});
    } else if (s_run.how == Change::Stagger && k >= 2 && k - 2 < s_run.unit_count) {
        lv_obj_remove_local_style_prop(s_run.units[k - 2], LV_STYLE_OPA, 0);  // the first frame was the ground's
        bake::now();
    }
    s_run.frame_at = s_frames;
    s_run.ms_at    = lv_tick_get();
}

void reset_motion(lv_obj_t *page)
{
    lv_obj_remove_local_style_prop(page, LV_STYLE_TRANSLATE_X, 0);
    lv_obj_remove_local_style_prop(page, LV_STYLE_OPA, 0);
}

#if defined(ESP_PLATFORM)
// LVGL drawing nothing meanwhile: pausing its timer is not enough, as anything
// marked for drawing, a button springing back under the lifting finger, wakes
// it, and its frames then came between the slide's. Put off rather than paused.
std::uint32_t s_refr_period = 0;

void hold_drawing(bool hold)
{
    lv_timer_t *t = lv_display_get_refr_timer(lv_display_get_default());
    if (hold) {
        s_refr_period = t->period;
        lv_timer_set_period(t, 60000);
        lv_timer_reset(t);
    } else if (s_refr_period != 0) {
        lv_timer_set_period(t, s_refr_period);
        s_refr_period = 0;
        lv_timer_ready(t);
    }
}

void end_glide()
{
    board::glide_end();
    s_gliding = false;
    hold_drawing(false);
    if (s_glide_dropped.x1 <= s_glide_dropped.x2) {
        lv_obj_invalidate_area(lv_screen_active(), &s_glide_dropped);  // what changed under the slide
    }
}

#endif

void finish()
{
    if (!s_run.active) {
        return;
    }
    if (s_run.stepper != nullptr) {
        lv_timer_delete(s_run.stepper);
        s_run.stepper = nullptr;
    }
    lv_anim_delete(&s_run, nullptr);
#if defined(ESP_PLATFORM)
    if (board::glide_held()) {  // cut short: the new page at rest, and LVGL drawing again
        end_glide();
    }
#endif
    for (int i = 0; i < s_run.unit_count; ++i) {
        lv_obj_remove_local_style_prop(s_run.units[i], LV_STYLE_OPA, 0);
    }
    if (s_run.from >= 0 && whole(s_run.how)) {
        // the old page is off the screen or clear by now, the new one where it rests
        quiet(true);
        lv_obj_set_hidden(s_pages[s_run.from].obj, true);
        reset_motion(s_pages[s_run.from].obj);
        reset_motion(s_pages[s_run.to].obj);
        quiet(false);
    }
    if (s_run.filtering) {
        if (s_run.step < s_run.steps) {
            open_to(s_run.region);
        }
        s_run.filtering = false;
    }
    const std::uint64_t end = perf::now_us();
    std::snprintf(s_report, sizeof s_report,
                  "%s: %u frames in %.0f ms, started in %.1f ms (pages told %.1f, shadows %.1f), first frame out %.1f ms after",
                  name_of(s_run.how), static_cast<unsigned>(s_run.frames),
                  static_cast<double>(end - s_run.began_us) / 1000.0,
                  static_cast<double>(s_run.ready_us - s_run.began_us) / 1000.0,
                  static_cast<double>(s_run.told_us) / 1000.0, static_cast<double>(s_run.bake_us) / 1000.0,
                  s_run.first_us != 0 ? static_cast<double>(s_run.first_us - s_run.ready_us) / 1000.0 : 0.0);
    s_run = Run{};
    ++s_run_id;
}

void on_step(lv_timer_t *)
{
    // a band a frame, or on regardless if no frame comes
    if (s_frames == s_run.frame_at && lv_tick_elaps(s_run.ms_at) < 150) {
        return;
    }
    if (s_run.step >= s_run.steps) {
        finish();
    } else {
        step();
    }
}

void slide(lv_anim_t *, int32_t p)
{
    // a carousel: the new page in from the side its item lies on in the dock
    const std::int32_t d = (size::screen_w - PAGES.x1) * (s_run.to < s_run.from ? -1 : 1);
    if (s_run.how == Change::Slide) {
        lv_obj_set_style_translate_x(s_pages[s_run.from].obj, -d * p / PROGRESS, 0);
        lv_obj_set_style_translate_x(s_pages[s_run.to].obj, d - d * p / PROGRESS, 0);
    } else {
        lv_obj_set_style_opa(s_pages[s_run.from].obj, static_cast<lv_opa_t>(255 - 255 * p / PROGRESS), 0);
        lv_obj_set_style_opa(s_pages[s_run.to].obj, static_cast<lv_opa_t>(255 * p / PROGRESS), 0);
    }
}

// The tiles to bring one by one: a page's own, and a grid's widgets, nearest the chosen item first.
void gather(lv_obj_t *page, std::int32_t cy)
{
    const std::uint32_t n = lv_obj_get_child_count(page);
    for (std::uint32_t i = 0; i < n && s_run.unit_count < MAX_UNITS; ++i) {
        lv_obj_t *c = lv_obj_get_child(page, static_cast<std::int32_t>(i));
        if (lv_obj_is_hidden(c)) {
            continue;
        }
        if (std::find(s_grids, s_grids + s_grid_count, c) != s_grids + s_grid_count) {
            gather(c, cy);
        } else {
            s_run.units[s_run.unit_count++] = c;
        }
    }
    const auto dist = [cy](lv_obj_t *o) {
        lv_area_t a;
        lv_obj_get_coords(o, &a);
        const std::int32_t dx = a.x1 - PAGES.x1, dy = (a.y1 + a.y2) / 2 - cy;
        return dx * dx + dy * dy;
    };
    std::stable_sort(s_run.units, s_run.units + s_run.unit_count,
                     [&](lv_obj_t *a, lv_obj_t *b) { return dist(a) < dist(b); });
}

// --- the dock's mark ------------------------------------------------------

void light(Page &p, bool on)
{
    if (p.item == nullptr || p.lit == on) {
        return;
    }
    // the outlined glyph and the filled one both there, one shown: a font
    // changed under the moving mark laid the dock out again, a slow frame
    p.lit = on;
    lv_obj_set_style_text_color(p.item, on ? colour(Ink::OnAccent) : colour(Ink::Primary), 0);
    lv_obj_set_style_text_opa(lv_obj_get_child(p.item, 0), on ? LV_OPA_TRANSP : colours().ink2_opa, 0);
    lv_obj_set_style_text_opa(lv_obj_get_child(p.item, 1), on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

struct Slot {
    std::int32_t x, y, w, h;
};
Slot s_mark_from{}, s_mark_to{}, s_mark_at{};

// The item the mark is over is lit: it changes over as the mark passes its middle.
void light_under_mark()
{
    const std::int32_t cx = s_mark_at.x + s_mark_at.w / 2, cy = s_mark_at.y + s_mark_at.h / 2;
    for (Page &p : s_pages) {
        if (p.item == nullptr) {
            continue;
        }
        const std::int32_t x = lv_obj_get_x(p.item), y = lv_obj_get_y(p.item);
        light(p, !lv_obj_is_hidden(s_mark) && cx >= x && cx < x + lv_obj_get_width(p.item) && cy >= y &&
                     cy < y + lv_obj_get_height(p.item));
    }
}

void place_mark(lv_anim_t *, int32_t p)
{
    const auto lerp = [p](std::int32_t a, std::int32_t b) { return a + (b - a) * p / PROGRESS; };
    s_mark_at = {lerp(s_mark_from.x, s_mark_to.x), lerp(s_mark_from.y, s_mark_to.y), lerp(s_mark_from.w, s_mark_to.w),
                 lerp(s_mark_from.h, s_mark_to.h)};
    lv_obj_set_pos(s_mark, s_mark_at.x, s_mark_at.y);
    lv_obj_set_size(s_mark, s_mark_at.w, s_mark_at.h);
    light_under_mark();
}

constexpr std::uint32_t MARK_MS      = 150;  // the dock's mark from one item to another
constexpr std::uint32_t MARK_LEAD_MS = MARK_MS;  // there as the page is drawn, which holds everything up

int  s_mark_target = -1;  // where the mark is, or is on its way to: ahead of the page shown, after a tap
bool s_placing     = false;  // in mark(), whose laying out the dock answers, and must not move it ahead of time

void mark(int index, bool animate)
{
    if (animate && index == s_mark_target && lv_anim_get(s_mark, nullptr) != nullptr) {
        return;  // already on its way there, as the press sent it
    }
    s_mark_target = index;
    struct Placing {
        Placing() { s_placing = true; }
        ~Placing() { s_placing = false; }
    } placing;
    lv_obj_t *item = index >= 0 ? s_pages[index].item : nullptr;
    if (s_mark == nullptr || item == nullptr || lv_obj_get_parent(item) != lv_obj_get_parent(s_mark)) {
        if (s_mark != nullptr) {
            lv_obj_set_hidden(s_mark, true);
            light_under_mark();
        }
        return;
    }
    lv_obj_update_layout(item);
    s_mark_to = {lv_obj_get_x(item), lv_obj_get_y(item), lv_obj_get_width(item), lv_obj_get_height(item)};
    lv_anim_delete(s_mark, nullptr);
    if (!animate || lv_obj_is_hidden(s_mark)) {
        lv_obj_set_hidden(s_mark, false);
        s_mark_from = s_mark_to;
        place_mark(nullptr, PROGRESS);
        return;
    }
    s_mark_from = s_mark_at;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_mark);
    lv_anim_set_values(&a, 0, PROGRESS);
    lv_anim_set_custom_exec_cb(&a, place_mark);
    lv_anim_set_duration(&a, MARK_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// A tap on the dock: the mark sets off at once, over the dock alone, which
// draws in a few milliseconds; the page follows once it is most of the way,
// as drawing a page holds everything else up for a few hundred.
lv_timer_t *s_going = nullptr;
char        s_mark_report[200] = "";

void go_to(int index)
{
    if (index == s_shown && s_going == nullptr) {
        return;
    }
    mark(index, true);
    perf::begin();
    if (s_going != nullptr) {
        lv_timer_delete(s_going);
    }
    s_going = lv_timer_create(
        [](lv_timer_t *t) {
            const int to = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_timer_get_user_data(t)));
            lv_timer_delete(t);
            s_going = nullptr;
            const perf::Stats glide = perf::end();
            const std::uint16_t *each = nullptr;
            const int n = std::min(perf::frames(&each), 12);
            int at = std::snprintf(s_mark_report, sizeof s_mark_report, "mark: %u frames, worst %.0f ms:",
                                   static_cast<unsigned>(glide.frames), static_cast<double>(glide.worst_ms));
            for (int i = 0; i < n && at < static_cast<int>(sizeof s_mark_report) - 6; ++i) {
                at += std::snprintf(s_mark_report + at, sizeof s_mark_report - at, " %u", static_cast<unsigned>(each[i]));
            }
            show_page(to);
        },
        MARK_LEAD_MS, reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
}

void restyle_mark(void *)
{
    lv_obj_set_style_bg_color(s_mark, colour(Ink::Accent), 0);
    for (Page &p : s_pages) {
        if (p.item != nullptr) {
            const bool was = p.lit;
            p.lit          = !was;
            light(p, was);
        }
    }
}

// --- widgets --------------------------------------------------------------

struct Grid : InPsram {
    int           columns = 4;
    std::uint32_t taken[32]{};  // a bit for each cell, a word for each row
    int           rows = 0;
};

constexpr std::int32_t edge_of(int i, int n, std::int32_t length)
{
    return i * (length + space::l) / n;
}

constexpr int GRID_ROWS = 3;  // to a page's height

void put(lv_obj_t *t, int columns, int col, int row, int w, int h)
{
    const std::int32_t x = edge_of(col, columns, inset::width), y = edge_of(row, GRID_ROWS, inset::height);
    const std::int32_t tw = edge_of(col + w, columns, inset::width) - space::l - x;
    const std::int32_t th = edge_of(row + h, GRID_ROWS, inset::height) - space::l - y;
    if (lv_obj_get_x(t) != x || lv_obj_get_y(t) != y) {
        lv_obj_set_pos(t, x, y);
    }
    if (lv_obj_get_width(t) != tw || lv_obj_get_height(t) != th) {
        lv_obj_set_size(t, tw, th);
    }
}

bool fits(const Grid &g, int col, int row, int w, int h)
{
    if (col + w > g.columns || row + h > static_cast<int>(std::size(g.taken))) {
        return false;
    }
    const std::uint32_t bits = ((1u << w) - 1) << col;
    for (int r = row; r < row + h; ++r) {
        if ((g.taken[r] & bits) != 0) {
            return false;
        }
    }
    return true;
}

lv_style_t s_widget_pressed;

void restyle_widgets(void *)
{
    lv_style_set_bg_color(&s_widget_pressed, lv_color_hex(colours().surface_hi));
}

}  // namespace

lv_obj_t *page()
{
    hooks();
    lv_obj_t *p = lv_obj_create(layer(Layer::Content));
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, size::screen_w, size::screen_h);
    lv_obj_set_style_pad_top(p, inset::top, 0);
    lv_obj_set_style_pad_left(p, inset::left, 0);
    lv_obj_set_style_pad_right(p, inset::right, 0);
    lv_obj_set_style_pad_bottom(p, inset::bottom, 0);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_elastic(p, false);
    lv_obj_set_scroll_chain(p, false);
    lv_obj_set_hidden(p, true);
    return p;
}

void set_change(Change how) { s_how = how; }
Change change() { return s_how; }

lv_obj_t *dock_item(lv_obj_t *dock, const char *glyph)
{
    if (s_mark == nullptr || lv_obj_get_parent(s_mark) != dock) {
        s_mark = lv_obj_create(dock);
        lv_obj_remove_style_all(s_mark);
        lv_obj_set_style_bg_opa(s_mark, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_mark, 24, 0);
        lv_obj_set_ignore_layout(s_mark, true);
        lv_obj_set_clickable(s_mark, false);
        lv_obj_set_hidden(s_mark, true);
        lv_obj_move_to_index(s_mark, 0);
        restyle_mark(nullptr);
        on_restyle(s_mark, restyle_mark, nullptr);
        // the dock laid out again, as a button of it comes or goes: the mark to
        // where its item now is, the one it went to after a tap and not the
        // page still shown, which pulled it back as the item it reached lit up
        lv_obj_add_event_cb(
            dock,
            [](lv_event_t *) {
                const int at = s_mark_target >= 0 ? s_mark_target : s_shown;
                if (at >= 0 && !s_placing && !lv_anim_get(s_mark, nullptr)) {
                    mark(at, false);
                }
            },
            LV_EVENT_LAYOUT_CHANGED, nullptr);
    }
    lv_obj_t *item = button(dock, Button::Flat, glyph, nullptr);
    lv_obj_set_size(item, 88, 80);
    // no give under the finger: the mark is what answers it, and a scaled item
    // is drawn through a layer of its own, which held up the mark's frames
    lv_obj_set_style_transform_scale_x(item, LV_SCALE_NONE, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(item, LV_SCALE_NONE, LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 24, 0);
    lv_obj_t *outlined = lv_obj_get_child(item, 0);
    lv_obj_set_style_text_font(outlined, fonts::icon(34, false), 0);
    lv_obj_set_style_text_opa(outlined, colours().ink2_opa, 0);
    lv_obj_t *filled = icon(item, glyph, 34, true);
    lv_obj_set_style_text_opa(filled, LV_OPA_TRANSP, 0);
    for (lv_obj_t *g : {outlined, filled}) {
        lv_obj_set_ignore_layout(g, true);
        lv_obj_center(g);
    }
    return item;
}

int add_page(lv_obj_t *item, void (*build)(lv_obj_t *page))
{
    if (s_page_count == MAX_PAGES) {
        return -1;
    }
    const int index = s_page_count++;
    Page     &p     = s_pages[index];
    p.obj   = page();
    p.item  = item;
    p.build = build;
    if (item != nullptr) {
        // as the finger lands rather than as it lifts: the page is there a tap's length sooner
        lv_obj_add_event_cb(
            item, [](lv_event_t *e) { go_to(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)))); },
            LV_EVENT_PRESSED, reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    }
    return index;
}

int shown_page() { return s_shown; }

void on_page_shown(void (*cb)(int index)) { s_told = cb; }

const char *change_report()
{
    static char both[sizeof s_report + sizeof s_mark_report + 2];
    std::snprintf(both, sizeof both, "%s\n%s", s_report, s_mark_report);
    return both;
}

void hide_pages()
{
    finish();
    if (s_shown >= 0 && !lv_obj_is_hidden(s_pages[s_shown].obj)) {
        lv_obj_set_hidden(s_pages[s_shown].obj, true);
    }
    mark(-1, false);
    edge_follows(layer(Layer::Content));
}

#if defined(ESP_PLATFORM)
std::int32_t  s_glide_w  = 0;
std::uint32_t s_glide_at = 0;
std::uint64_t s_glide_show_us = 0;

void glide_tick(lv_timer_t *)
{
    lv_anim_t a;  // only to be read by the spring
    lv_anim_init(&a);
    a.start_value = 0;
    a.end_value   = s_glide_w;
    a.duration    = static_cast<std::int32_t>(motion::pop_ms);
    a.act_time    = std::min<std::int32_t>(static_cast<std::int32_t>(lv_tick_elaps(s_glide_at)), a.duration);
    const bool last = a.act_time >= a.duration;
    if (board::glide_compose(last ? s_glide_w : motion::spring(&a))) {
        const std::uint64_t drawn = perf::now_us();
        lv_refr_now(lv_display_get_default());
        board::glide_show();
        s_glide_show_us += perf::now_us() - drawn;
    }
    if (!last) {
        return;
    }
    lv_timer_delete(s_run.stepper);
    s_run.stepper = nullptr;
    end_glide();
    const board::GlideTimes t = board::glide_times();
    const double f = t.frames != 0 ? t.frames * 1000.0 : 1.0;
    std::snprintf(s_report, sizeof s_report,
                  "glide: %u frames in %.0f ms, the new page drawn in %.1f ms; a frame: %.1f ms copying, %.1f of it waiting, %.1f drawing and showing",
                  static_cast<unsigned>(t.frames), static_cast<double>(perf::now_us() - s_run.began_us) / 1000.0,
                  static_cast<double>(s_run.ready_us - s_run.began_us) / 1000.0, static_cast<double>(t.copy_us) / f,
                  static_cast<double>(t.wait_us) / f, static_cast<double>(s_glide_show_us) / f);
    s_glide_show_us = 0;
    s_run = Run{};
    ++s_run_id;
}

// On the panel a slide is copied, not drawn: the new page drawn once, at
// rest, into a frame held back, then laid beside the old one a step a frame.
// Between the bar and the bottom only, so the bar and the dock stay.
bool glide(int from, int to)
{
    // clear of the dock's and the bar's shadows, which would otherwise travel
    // with the page that carries them in
    const lv_area_t region{PAGES.x1 + 8, inset::top - 2, size::screen_w - 1, size::screen_h - 1};
    hooks();
    if (!board::glide_begin(region, to < from)) {
        return false;
    }
    quiet(true);
    if (s_told != nullptr) {
        s_told(to);
    }
    lv_obj_set_hidden(s_pages[from].obj, true);
    lv_obj_set_hidden(s_pages[to].obj, false);
    s_shown = to;
    quiet(false);
    edge_follows(s_pages[to].obj);
    lv_display_t *disp = lv_display_get_default();
    lv_obj_invalidate_area(lv_screen_active(), &region);
    lv_refr_now(disp);
    if (!board::glide_held()) {
        board::glide_end();
        mark(to, true);
        return true;
    }
    hold_drawing(true);
    s_gliding       = true;
    s_glide_region  = region;
    s_glide_dropped = NOTHING;
    mark(to, true);  // round the region LVGL still draws, into each step's frame
    s_run.ready_us = perf::now_us();
    s_glide_w      = lv_area_get_width(&region);
    s_glide_at     = lv_tick_get();
    // a timer rather than an animation: each step draws, and drawing runs
    // LVGL's animations, which must not happen inside one of theirs
    s_run.stepper = lv_timer_create(glide_tick, 1, nullptr);
    return true;
}
#endif

void show_page(int index, bool animate)
{
    if (index < 0 || index >= s_page_count) {
        return;
    }
    finish();
    Page &to = s_pages[index];
    if (index == s_shown && !lv_obj_is_hidden(to.obj)) {
        return;
    }
    s_run.began_us = perf::now_us();
    build(to);
    const int from = s_shown >= 0 && !lv_obj_is_hidden(s_pages[s_shown].obj) ? s_shown : -1;
    lv_obj_update_layout(to.obj);
    const Change how = !animate ? Change::Instant : from < 0 && whole(s_how) ? Change::Instant : s_how;
#if defined(ESP_PLATFORM)
    if (how == Change::Slide) {
        s_run.active = true;
        s_run.how    = how;
        s_run.from   = from;
        s_run.to     = index;
        if (glide(from, index)) {
            return;
        }
        s_run = Run{};
        s_run.began_us = perf::now_us();
    }
#endif

    s_run.active = true;
    s_run.how    = how;
    s_run.from   = from;
    s_run.to     = index;
    s_run.centre = item_centre(index);
    lv_area_t region = from >= 0 ? drawn(s_pages[from].obj) : PAGES;
    region           = span(region, drawn(to.obj));
    s_run.region     = region.x1 <= region.x2 ? region : PAGES;
    s_run.open       = NOTHING;

    // all of it made without marking anything for drawing: the change draws
    // the region itself, as it opens
    quiet(true);
    const std::uint64_t told = perf::now_us();
    if (s_told != nullptr) {
        s_told(index);
    }
    s_run.told_us = perf::now_us() - told;
    if (from >= 0 && !whole(how)) {
        lv_obj_set_hidden(s_pages[from].obj, true);
    }
    if (how == Change::Stagger) {
        gather(to.obj, s_run.centre);
        for (int i = 0; i < s_run.unit_count; ++i) {
            lv_obj_set_style_opa(s_run.units[i], LV_OPA_TRANSP, 0);  // not hidden: a page laid out by flex would close up
        }
    }
    if (whole(how)) {
        slide(nullptr, 0);
    }
    lv_obj_set_hidden(to.obj, false);
    s_shown = index;
    quiet(false);

    s_run.filtering = !whole(how);
    const std::uint64_t t = perf::now_us();
    // the new page's shadows painted into the ground now, while it is not
    // drawn yet: its bands then draw them as a copy of the ground
    if (!whole(how)) {
        bake::now();
    }
    s_run.bake_us = perf::now_us() - t;
    mark(index, animate);
    // The dock drawn again with the page: the buffer the page goes into could
    // still hold the mark where it set off from, and it showed there for a frame.
    if (s_mark != nullptr) {
        lv_obj_invalidate(lv_obj_get_parent(s_mark));
    }
    edge_follows(to.obj);

    if (whole(how)) {
        lv_obj_invalidate_area(lv_screen_active(), &s_run.region);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, &s_run);
        lv_anim_set_values(&a, 0, PROGRESS);
        lv_anim_set_custom_exec_cb(&a, slide);
        lv_anim_set_duration(&a, how == Change::Slide ? motion::pop_ms : motion::fade_ms);
        lv_anim_set_path_cb(&a, how == Change::Slide ? motion::spring : lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, [](lv_anim_t *) {
            lv_async_call(
                [](void *id) {
                    if (static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(id)) == s_run_id) {
                        finish();
                    }
                },
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(s_run_id)));
        });
        lv_anim_start(&a);
    } else {
        s_run.steps = banded(how) ? BANDS : 1 + s_run.unit_count;
        if (banded(how)) {
            step();
        } else {
            open_to(s_run.region);
            s_run.step     = 1;
            s_run.frame_at = s_frames;
            s_run.ms_at    = lv_tick_get();
        }
        s_run.stepper = lv_timer_create(on_step, 1, nullptr);
    }
    s_run.ready_us = perf::now_us();
}

lv_obj_t *widget_grid(lv_obj_t *page, int columns)
{
    static bool styled = false;
    lv_obj_t   *grid   = lv_obj_create(page);
    lv_obj_remove_style_all(grid);
    lv_obj_set_scrollable(grid, false);
    lv_obj_set_clickable(grid, false);
    lv_obj_set_size(grid, inset::width, 0);
    auto *g    = new Grid{};
    g->columns = std::clamp(columns, 1, 8);
    lv_obj_set_user_data(grid, g);
    lv_obj_add_event_cb(
        grid,
        [](lv_event_t *e) {
            lv_obj_t *gone = lv_event_get_target_obj(e);
            delete static_cast<Grid *>(lv_obj_get_user_data(gone));
            s_grid_count = static_cast<int>(std::remove(s_grids, s_grids + s_grid_count, gone) - s_grids);
        },
        LV_EVENT_DELETE, nullptr);
    if (s_grid_count < static_cast<int>(std::size(s_grids))) {
        s_grids[s_grid_count++] = grid;
    }
    if (!styled) {
        styled = true;
        lv_style_init(&s_widget_pressed);
        restyle_widgets(nullptr);
        on_restyle(grid, restyle_widgets, nullptr);
    }
    return grid;
}

lv_obj_t *widget(lv_obj_t *grid, Span span_of, const char *caption)
{
    auto     *g = static_cast<Grid *>(lv_obj_get_user_data(grid));
    const int w = span_of == Span::Wide || span_of == Span::Big ? 2 : 1;
    const int h = span_of == Span::Tall || span_of == Span::Big ? 2 : 1;
    int col = 0, row = 0;
    for (bool found = false; !found;) {
        found = fits(*g, col, row, w, h);
        if (!found && ++col >= g->columns) {
            col = 0;
            ++row;
        }
        if (row >= static_cast<int>(std::size(g->taken))) {
            return nullptr;
        }
    }
    for (int r = row; r < row + h; ++r) {
        g->taken[r] |= ((1u << w) - 1) << col;
    }
    g->rows = std::max(g->rows, row + h);

    lv_obj_t *t = card(grid, caption);
    put(t, g->columns, col, row, w, h);
    lv_obj_set_clickable(t, false);
    lv_obj_t *body = lv_obj_create(t);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_scrollable(body, false);
    lv_obj_set_clickable(body, false);
    lv_obj_set_height(grid, edge_of(g->rows, GRID_ROWS, inset::height) - space::l);
    return body;
}

void widget_place(lv_obj_t *body, int col, int row, int w, int h)
{
    lv_obj_t  *t = lv_obj_get_parent(body);
    const auto *g = static_cast<const Grid *>(lv_obj_get_user_data(lv_obj_get_parent(t)));
    put(t, g->columns, col, row, w, h);
    lv_obj_update_layout(t);
}

namespace {
void unclickable(lv_obj_t *obj)
{
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *c = lv_obj_get_child(obj, static_cast<std::int32_t>(i));
        lv_obj_set_clickable(c, false);
        unclickable(c);
    }
}
}  // namespace

// Once it is filled: a tap anywhere on it is a tap on the widget.
void widget_opens(lv_obj_t *body, int page_index)
{
    lv_obj_t *t = lv_obj_get_parent(body);
    unclickable(t);
    lv_obj_set_clickable(t, true);
    lv_obj_add_style(t, &s_widget_pressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(
        t, [](lv_event_t *e) { go_to(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)))); },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(page_index)));
}

}  // namespace ui::kit
