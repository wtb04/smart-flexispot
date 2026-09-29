#include "ui_internal.h"

#include <numbers>

namespace ui::detail {
namespace {
lv_obj_t     *s_splash                      = nullptr;
constexpr int SPLASH_SEGMENTS               = 12;
lv_obj_t     *s_splash_seg[SPLASH_SEGMENTS] = {};
lv_obj_t     *s_splash_about                = nullptr;

// The tabs, in their order along the bottom.
constexpr const char *SPLASH_ICONS[] = {LV_SYMBOL_HOME, "", "", "", LV_SYMBOL_SETTINGS};
const lv_image_dsc_t *const SPLASH_IMAGES[]   = {nullptr, &icons::plane_icon, &icons::calendar_icon,
                                                 &icons::timer_icon, nullptr};
constexpr int               SPLASH_ICON_COUNT = static_cast<int>(std::size(SPLASH_ICONS));
constexpr std::int32_t      SPLASH_CHIP       = 56;
constexpr float             SPLASH_REACH      = 158.0f;
constexpr float             SPLASH_SPREAD     = 68.0f; // degrees either side of straight up
lv_obj_t                   *s_splash_icon[SPLASH_ICON_COUNT] = {};
lv_point_t                  s_splash_origin = {}; // the middle of the desktop at its lowest

struct SplashStep {
    const char *key;
    const char *name;
    lv_obj_t   *state;
};
SplashStep    s_splash_steps[]  = {{"desk", "Desk", nullptr},
                                   {"network", "Network", nullptr},
                                   {"", "Home Assistant", nullptr}}; // up when splash_done()
constexpr int SPLASH_STEP_COUNT = static_cast<int>(std::size(s_splash_steps));
lv_obj_t     *s_splash_top      = nullptr;
lv_obj_t     *s_splash_leg[2]   = {};
lv_timer_t   *s_splash_guard    = nullptr;
bool          s_splash_up       = false;
lv_timer_t   *s_splash_tick     = nullptr;
std::uint32_t s_splash_start    = 0;
bool          s_splash_ready    = false;
std::uint32_t s_splash_tick_at  = 0;
float         s_splash_at       = 0.0f;  // how far into the splash's own twelve seconds
float         s_splash_rush     = 0.0f;  // its pace once startup is ready, if faster

constexpr std::int32_t DESK_W            = 300;
constexpr std::int32_t DESK_H            = 160;
constexpr std::int32_t DESK_BAR          = 16;
constexpr std::int32_t DESK_LOWEST_H     = 58;  // floor to top of the desktop
constexpr std::int32_t DESK_LOW          = DESK_H - DESK_LOWEST_H;
constexpr std::int32_t DESK_HIGH         = 6;
constexpr std::int32_t DESK_FOOT_W       = 88;
constexpr std::int32_t DESK_FOOT_H       = 12;
constexpr std::int32_t DESK_FOOT_INSET   = 8;
constexpr std::int32_t DESK_LEG_INSET    = 44;
constexpr std::int32_t DESK_LEG_FIRST_H  = 10;
constexpr std::int32_t SPLASH_BAR_RADIUS = 3;

constexpr std::int32_t STEPS_W = 360;
constexpr std::int32_t SEG_W   = 30;
constexpr std::int32_t SEG_H   = 10;
constexpr std::int32_t SEG_GAP = 8;

// Driven by the clock alone, so a slow frame does not slow the splash. Twelve
// seconds of movement, played in eight, and what is left of it in a moment
// once startup is ready.
constexpr std::uint32_t SPLASH_MS       = 12000;
constexpr float         SPLASH_PACE     = 12000.0f / 8000.0f;
constexpr float         SPLASH_RUSH_MS  = 600.0f;
constexpr std::uint32_t FRAME_MS        = 16;
constexpr std::uint32_t SPLASH_HOLD_MS  = 400;
constexpr float         RISE_FROM_MS    = 300.0f;
constexpr float         RISE_MS         = 5200.0f;
constexpr float         ICONS_FROM_MS   = 6000.0f;
constexpr float         ICON_STAGGER_MS = 1000.0f;
constexpr float         ICON_FLIGHT_MS  = 600.0f;
// Each icon is full size by two fifths of its flight and opaque by a third.
constexpr float         ICON_GROW_RATE  = 2.5f;
constexpr float         ICON_FADE_RATE  = 3.0f;
constexpr float         BACK_OVERSHOOT  = 1.70158f;

constexpr float COVER             = LV_OPA_COVER;
constexpr float PI                = std::numbers::pi_v<float>;
constexpr float HALF_TURN_DEGREES = 180.0f;

void splash_hide(lv_anim_t *)
{
    lv_obj_set_hidden(s_splash, true);
    if (s_splash_tick != nullptr) {
        lv_timer_delete(s_splash_tick);
        s_splash_tick = nullptr;
    }
}

// LVGL redraws whatever a style is set on, changed or not, so each frame sets
// only what moved.
void set_colour_once(lv_obj_t *obj, lv_color_t colour)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), colour)) {
        lv_obj_set_style_bg_color(obj, colour, 0);
    }
}

void set_pos_once(lv_obj_t *obj, std::int32_t x, std::int32_t y)
{
    if (lv_obj_get_x_aligned(obj) != x || lv_obj_get_y_aligned(obj) != y) {
        lv_obj_set_pos(obj, x, y);
    }
}

float ease_in_out(float t)
{
    return 0.5f - 0.5f * std::cos(t * PI);
}

float ease_out_back(float t)
{
    return 1.0f + (BACK_OVERSHOOT + 1.0f) * std::pow(t - 1.0f, 3.0f) +
           BACK_OVERSHOOT * std::pow(t - 1.0f, 2.0f);
}

float radians(float degrees)
{
    return degrees * PI / HALF_TURN_DEGREES;
}

std::int32_t rounded(float value)
{
    return static_cast<std::int32_t>(std::lround(value));
}

float phase(std::uint32_t elapsed, float from_ms, float length_ms)
{
    return std::clamp((static_cast<float>(elapsed) - from_ms) / length_ms, 0.0f, 1.0f);
}

void light_segments(std::uint32_t elapsed)
{
    const float run = std::min(static_cast<float>(elapsed) / SPLASH_MS, 1.0f);
    for (int i = 0; i < SPLASH_SEGMENTS; ++i) {
        const float lit = std::clamp(run * SPLASH_SEGMENTS - static_cast<float>(i), 0.0f, 1.0f);
        set_colour_once(s_splash_seg[i],
                        lv_color_mix(lv_color_hex(theme::primary), lv_color_hex(theme::panel),
                                     static_cast<std::uint8_t>(COVER * lit)));
    }
}

/** Where the desktop stands: DESK_LOW at the start, rising to DESK_HIGH. */
std::int32_t desktop_y(std::uint32_t elapsed)
{
    const float rise = ease_in_out(phase(elapsed, RISE_FROM_MS, RISE_MS));
    return DESK_LOW - rounded((DESK_LOW - DESK_HIGH) * rise);
}

void fly_icon(int i, float t, std::int32_t top)
{
    const float back  = ease_out_back(t);
    const float angle = radians(-SPLASH_SPREAD + 2.0f * SPLASH_SPREAD * static_cast<float>(i) /
                                                     (SPLASH_ICON_COUNT - 1));
    const float ox    = static_cast<float>(s_splash_origin.x);
    const float oy    = static_cast<float>(s_splash_origin.y + top - DESK_LOW);
    const float to_x  = ox + SPLASH_REACH * std::sin(angle);
    const float to_y  = static_cast<float>(s_splash_origin.y + DESK_HIGH - DESK_LOW) -
                       SPLASH_REACH * std::cos(angle);
    set_pos_once(s_splash_icon[i], rounded(ox + (to_x - ox) * back) - SPLASH_CHIP / 2,
                 rounded(oy + (to_y - oy) * back) - SPLASH_CHIP / 2);

    const auto scale =
        static_cast<std::int32_t>(LV_SCALE_NONE * std::min(t * ICON_GROW_RATE, 1.0f));
    if (lv_obj_get_style_transform_scale_x(s_splash_icon[i], LV_PART_MAIN) != scale) {
        lv_obj_set_style_transform_scale(s_splash_icon[i], scale, 0);
    }
    const auto opa = static_cast<lv_opa_t>(COVER * std::min(t * ICON_FADE_RATE, 1.0f));
    if (lv_obj_get_style_opa(s_splash_icon[i], LV_PART_MAIN) != opa) {
        lv_obj_set_style_opa(s_splash_icon[i], opa, 0);
    }
}

void fly_icons(std::uint32_t elapsed, std::int32_t top)
{
    static bool landed[SPLASH_ICON_COUNT] = {};
    for (int i = 0; i < SPLASH_ICON_COUNT; ++i) {
        const float t =
            phase(elapsed, ICONS_FROM_MS + ICON_STAGGER_MS * static_cast<float>(i), ICON_FLIGHT_MS);
        if (t <= 0.0f || landed[i]) {
            continue;
        }
        landed[i] = t >= 1.0f;
        fly_icon(i, t, top);
    }
}

void stretch_legs(std::int32_t top)
{
    for (lv_obj_t *leg : s_splash_leg) {
        if (lv_obj_get_y_aligned(leg) != top + DESK_BAR) {
            lv_obj_set_y(leg, top + DESK_BAR);
            lv_obj_set_height(leg, DESK_H - DESK_BAR - DESK_FOOT_H - top);
        }
    }
}

void splash_frame(std::uint32_t elapsed)
{
    light_segments(elapsed);
    const std::int32_t top = desktop_y(elapsed);
    set_pos_once(s_splash_top, 0, top);
    fly_icons(elapsed, top);
    stretch_legs(top);
}

// One even movement rather than a lurch per step, which waits at the end if
// boot is slower.
constexpr std::uint32_t SPLASH_GUARD_MS  = 15000;
bool                    s_splash_leaving = false;

// LVGL draws the screen under the top layer even where the splash covers it, and
// Home Assistant filling the pages in cost a fifth of a second a frame.
constexpr int SPLASH_HIDDEN_MAX               = 24;
lv_obj_t     *s_splash_hid[SPLASH_HIDDEN_MAX] = {};
int           s_splash_hid_count              = 0;

// A cut, not a fade: a full-screen blend takes a quarter of a second a frame here.
void splash_leave()
{
    ESP_LOGI(TAG, "splash: left after %u ms", static_cast<unsigned>(lv_tick_elaps(s_splash_start)));
    s_splash_leaving = true;
    for (int i = 0; i < s_splash_hid_count; ++i) {
        lv_obj_set_hidden(s_splash_hid[i], false);
    }
    s_splash_hid_count = 0;
    splash_hide(nullptr);
}

void splash_animate(lv_timer_t *)
{
    const auto end  = static_cast<float>(SPLASH_MS + SPLASH_HOLD_MS);
    const auto real = static_cast<float>(lv_tick_elaps(s_splash_tick_at));
    s_splash_tick_at = lv_tick_get();
    if (s_splash_ready && s_splash_rush == 0.0f) {
        s_splash_rush = std::max(SPLASH_PACE, (end - s_splash_at) / SPLASH_RUSH_MS);
    }
    s_splash_at += real * (s_splash_ready ? s_splash_rush : SPLASH_PACE);
    s_splash_at = std::min(s_splash_at, s_splash_ready ? end : static_cast<float>(SPLASH_MS));
    splash_frame(static_cast<std::uint32_t>(s_splash_at));
    if (s_splash_at >= end && !s_splash_leaving) {
        splash_leave();
    }
}

void splash_expired(lv_timer_t *)
{
    s_splash_guard = nullptr;
    ESP_ERROR_CHECK_WITHOUT_ABORT(splash_done());
}

lv_obj_t *splash_bar(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h, std::uint32_t colour)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    theme::style_panel(bar, colour, SPLASH_BAR_RADIUS);
    lv_obj_set_clickable(bar, false);
    return bar;
}

lv_obj_t *make_bare_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_clickable(box, false);
    return box;
}

// Asked for from other tasks, drawn by apply_splash() on the LVGL task.
std::atomic<int>  s_steps_wanted{0};
std::atomic<bool> s_done_wanted{false};
int               s_steps_shown = 0;

void splash_mark(int current)
{
    for (int i = 0; i < SPLASH_STEP_COUNT; ++i) {
        lv_obj_t *state = s_splash_steps[i].state;
        theme::set_text(state, i < current ? "Ready" : i == current ? "Starting" : "Waiting");
        theme::set_text_color(state, i < current    ? theme::primary
                                     : i == current ? theme::text
                                                    : theme::secondary);
        lv_obj_set_style_text_opa(state, i > current ? LV_OPA_60 : LV_OPA_COVER, 0);
    }
}

lv_obj_t *build_splash_panel(const Layout &l)
{
    lv_obj_t *panel = lv_obj_create(s_splash);
    lv_obj_set_pos(panel, GAP, GAP);
    lv_obj_set_size(panel, l.screen_w - 2 * GAP, l.screen_h - 2 * GAP);
    theme::style_panel(panel, theme::panel, theme::radius::card);
    lv_obj_set_style_pad_all(panel, PANEL_PAD, 0);
    lv_obj_set_clickable(panel, false);
    return panel;
}

void build_flying_icons(lv_obj_t *hero)
{
    for (int i = 0; i < SPLASH_ICON_COUNT; ++i) {
        lv_obj_t *icon   = theme::make_chip(hero, SPLASH_ICONS[i], theme::type_value());
        s_splash_icon[i] = icon;
        lv_obj_set_size(icon, SPLASH_CHIP, SPLASH_CHIP);
        lv_obj_set_style_radius(icon, SPLASH_CHIP / 2, 0);
        lv_obj_set_style_transform_pivot_x(icon, SPLASH_CHIP / 2, 0);
        lv_obj_set_style_transform_pivot_y(icon, SPLASH_CHIP / 2, 0);
        lv_obj_set_style_opa(icon, LV_OPA_TRANSP, 0);
        lv_obj_t *glyph = lv_obj_get_child(icon, 0);
        if (SPLASH_IMAGES[i] != nullptr) {
            lv_obj_delete(glyph);
            glyph = lv_image_create(icon);
            lv_image_set_src(glyph, SPLASH_IMAGES[i]);
            lv_obj_set_style_image_recolor(glyph, lv_color_hex(theme::primary), 0);
            lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
            lv_obj_center(glyph);
        } else {
            theme::set_text_color(glyph, theme::primary);
            lv_obj_set_style_text_opa(glyph, LV_OPA_COVER, 0);
        }
        lv_obj_set_clickable(icon, false);
    }
}

void build_desk(lv_obj_t *hero, std::int32_t top)
{
    lv_obj_t *desk = make_bare_box(hero);
    lv_obj_set_size(desk, DESK_W, DESK_H);
    lv_obj_align(desk, LV_ALIGN_TOP_MID, 0, top);

    const std::int32_t foot_y = DESK_H - DESK_FOOT_H;
    splash_bar(desk, DESK_FOOT_INSET, foot_y, DESK_FOOT_W, DESK_FOOT_H, theme::panel);
    splash_bar(desk, DESK_W - DESK_FOOT_INSET - DESK_FOOT_W, foot_y, DESK_FOOT_W, DESK_FOOT_H,
               theme::panel);
    s_splash_leg[0] = splash_bar(desk, DESK_LEG_INSET, 0, DESK_BAR, DESK_LEG_FIRST_H, theme::panel);
    s_splash_leg[1] = splash_bar(desk, DESK_W - DESK_LEG_INSET - DESK_BAR, 0, DESK_BAR,
                                 DESK_LEG_FIRST_H, theme::panel);
    s_splash_top    = splash_bar(desk, 0, 0, DESK_W, DESK_BAR, theme::panel);
    lv_obj_set_style_radius(s_splash_top, DESK_BAR / 2, 0);
    theme::fill_accent(s_splash_top);
}

void build_titles(lv_obj_t *hero, std::int32_t y)
{
    lv_obj_align(theme::make_label(hero, "Smart Flexispot", theme::text, theme::type_display()),
                 LV_ALIGN_TOP_MID, 0, y);
    lv_obj_align(
        theme::make_label(hero, "by Wouter ten Brinke", theme::secondary, theme::type_body()),
        LV_ALIGN_TOP_MID, 0, y + theme::type_display()->line_height);
}

void build_progress_bar(lv_obj_t *hero, std::int32_t y)
{
    lv_obj_t *segments = make_bare_box(hero);
    lv_obj_set_size(segments, SPLASH_SEGMENTS * (SEG_W + SEG_GAP) - SEG_GAP, SEG_H);
    lv_obj_align(segments, LV_ALIGN_TOP_MID, 0, y);
    for (int i = 0; i < SPLASH_SEGMENTS; ++i) {
        s_splash_seg[i] =
            splash_bar(segments, i * (SEG_W + SEG_GAP), 0, SEG_W, SEG_H, theme::panel);
        lv_obj_set_style_radius(s_splash_seg[i], SEG_H / 2, 0);
    }
}

void toggle_about(lv_event_t *)
{
    lv_obj_set_hidden(s_splash_about, !lv_obj_is_hidden(s_splash_about));
}

void build_about(lv_obj_t *hero)
{
    char about[80];
    std::snprintf(about, sizeof(about), "Smart Flexispot, build %s",
                  esp_app_get_description()->version);
    s_splash_about = theme::make_label(hero, about, theme::secondary, theme::type_label());
    lv_obj_set_style_text_opa(s_splash_about, LV_OPA_60, 0);
    lv_obj_align(s_splash_about, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_splash_about, true);
    lv_obj_add_event_cb(s_splash, toggle_about, LV_EVENT_CLICKED, nullptr);
}

/** The icons, the desk, the name and the progress bar, as one block in the middle. */
void build_hero(lv_obj_t *panel, std::int32_t w, std::int32_t h)
{
    lv_obj_t *hero = theme::make_card(panel);
    lv_obj_set_pos(hero, 0, 0);
    lv_obj_set_size(hero, w, h);
    lv_obj_set_clickable(hero, false);

    const std::int32_t inner_w = w - 2 * theme::space::l;
    const std::int32_t arc_h   = static_cast<std::int32_t>(SPLASH_REACH) + SPLASH_CHIP / 2;
    const std::int32_t titles_h =
        theme::type_display()->line_height + theme::type_body()->line_height;
    const std::int32_t block =
        arc_h + DESK_H + theme::space::l + titles_h + theme::space::xl + SEG_H;
    const std::int32_t top = (h - 2 * theme::space::l - block) / 2 + arc_h;

    // Created before the desk, so they come out from behind it.
    build_flying_icons(hero);
    s_splash_origin = {inner_w / 2, top + DESK_LOW};
    build_desk(hero, top);

    const std::int32_t title_y = top + DESK_H + theme::space::l;
    build_titles(hero, title_y);
    build_progress_bar(hero, title_y + titles_h + theme::space::xl);
    build_about(hero);
}

void build_step_chip(lv_obj_t *card, const lv_image_dsc_t *image)
{
    lv_obj_t *chip = theme::make_chip(card, image != nullptr ? "" : LV_SYMBOL_HOME,
                                      fonts::size_28());
    lv_obj_set_clickable(chip, false);
    if (image != nullptr) {
        lv_obj_t *mark = lv_image_create(chip);
        lv_image_set_src(mark, image);
        lv_obj_set_style_image_recolor(mark, lv_color_hex(theme::secondary), 0);
        lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_image_opa(mark, theme::mark_opa, 0);
        lv_obj_center(mark);
    }
}

void build_steps(lv_obj_t *panel, std::int32_t x, std::int32_t h)
{
    const std::int32_t step_h = (h - (SPLASH_STEP_COUNT - 1) * GAP) / SPLASH_STEP_COUNT;
    const lv_image_dsc_t *const STEP_ICONS[SPLASH_STEP_COUNT] = {&icons::desk_icon,
                                                                 &icons::wifi_icon, nullptr};
    for (int i = 0; i < SPLASH_STEP_COUNT; ++i) {
        lv_obj_t *card = theme::make_card(panel);
        lv_obj_set_pos(card, x, i * (step_h + GAP));
        lv_obj_set_size(card, STEPS_W, step_h);
        lv_obj_set_clickable(card, false);
        build_step_chip(card, STEP_ICONS[i]);

        lv_obj_t *text = make_bare_box(card);
        lv_obj_set_size(text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(text, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, theme::space::xs, 0);
        theme::make_label(text, s_splash_steps[i].name, theme::secondary, theme::type_label());
        s_splash_steps[i].state = theme::make_label(text, "", theme::text, theme::type_title());
    }
}

void start_splash_timers()
{
    s_splash_start   = lv_tick_get();
    s_splash_tick_at = s_splash_start;
    s_splash_tick    = lv_timer_create(splash_animate, FRAME_MS, nullptr);

    s_splash_guard = lv_timer_create(splash_expired, SPLASH_GUARD_MS, nullptr);
    lv_timer_set_repeat_count(s_splash_guard, 1);
}
} // namespace

void build_splash()
{
    const Layout l = layout();

    s_splash = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_size(s_splash, l.screen_w, l.screen_h);
    theme::style_panel(s_splash, theme::background, 0);
    lv_obj_set_clickable(s_splash, true);
    s_splash_up = true;

    lv_obj_t          *panel   = build_splash_panel(l);
    const std::int32_t inner_w = l.screen_w - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t inner_h = l.screen_h - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t hero_w  = inner_w - STEPS_W - GAP;
    build_hero(panel, hero_w, inner_h);
    build_steps(panel, hero_w + GAP, inner_h);

    splash_mark(0);
    splash_frame(0);
    start_splash_timers();
}

void keep_under_splash()
{
    if (s_splash_leaving) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
        if (lv_obj_is_hidden(child)) {
            continue;
        }
        if (s_splash_hid_count == SPLASH_HIDDEN_MAX) {
            ESP_LOGW(TAG, "splash: more on the screen than it can keep hidden");
            return;
        }
        lv_obj_set_hidden(child, true);
        s_splash_hid[s_splash_hid_count++] = child;
    }
}

void apply_splash()
{
    if (!s_splash_up) {
        return;
    }
    if (s_done_wanted.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "splash: ready after %u ms",
                 static_cast<unsigned>(lv_tick_elaps(s_splash_start)));
        s_splash_up    = false;
        s_splash_ready = true;
        splash_mark(SPLASH_STEP_COUNT);
        if (s_splash_guard != nullptr) {
            lv_timer_delete(s_splash_guard);
            s_splash_guard = nullptr;
        }
        return;
    }
    const int steps = s_steps_wanted.load(std::memory_order_relaxed);
    if (steps > s_steps_shown) {
        s_steps_shown = steps;
        splash_mark(steps);
    }
}

} // namespace ui::detail

namespace ui {
using namespace detail;

esp_err_t splash_step(const char *label)
{
    for (int i = 0; i < SPLASH_STEP_COUNT; ++i) {
        if (label != nullptr && std::strcmp(label, s_splash_steps[i].key) == 0) {
            int wanted = s_steps_wanted.load(std::memory_order_relaxed);
            while (wanted < i + 1 && !s_steps_wanted.compare_exchange_weak(
                                         wanted, i + 1, std::memory_order_relaxed)) {
            }
        }
    }
    request_apply();
    return ESP_OK;
}

esp_err_t splash_done()
{
    // Told every two seconds for as long as everything is up; only the first counts.
    if (!s_done_wanted.exchange(true, std::memory_order_relaxed)) {
        request_apply();
    }
    return ESP_OK;
}
} // namespace ui
