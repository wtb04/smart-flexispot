#include "ui_internal.h"

namespace ui::detail {
namespace {
lv_obj_t     *s_splash                      = nullptr;
constexpr int SPLASH_SEGMENTS               = 12;
lv_obj_t     *s_splash_seg[SPLASH_SEGMENTS] = {};
lv_obj_t     *s_splash_about                = nullptr;

constexpr const char *SPLASH_ICONS[] = {LV_SYMBOL_HOME, "", LV_SYMBOL_BELL, "", LV_SYMBOL_SETTINGS};
const lv_image_dsc_t *const SPLASH_IMAGES[]   = {nullptr, &icons::calendar_icon, nullptr,
                                                 &icons::plane_icon, nullptr};
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

constexpr std::int32_t DESK_W    = 300;
constexpr std::int32_t DESK_H    = 160;
constexpr std::int32_t DESK_BAR  = 16;
constexpr std::int32_t DESK_LOW  = DESK_H - 58;
constexpr std::int32_t DESK_HIGH = 6;

void splash_hide(lv_anim_t *)
{
    lv_obj_set_hidden(s_splash, true);
    if (s_splash_tick != nullptr) {
        lv_timer_delete(s_splash_tick);
        s_splash_tick = nullptr;
    }
}

// Driven by the clock alone, so a slow frame does not slow the splash.
constexpr std::uint32_t SPLASH_MS = 12000;

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

void splash_frame(std::uint32_t elapsed)
{
    const float run = std::min(static_cast<float>(elapsed) / SPLASH_MS, 1.0f);
    for (int i = 0; i < SPLASH_SEGMENTS; ++i) {
        const float lit = std::clamp(run * SPLASH_SEGMENTS - static_cast<float>(i), 0.0f, 1.0f);
        set_colour_once(s_splash_seg[i],
                        lv_color_mix(lv_color_hex(theme::primary), lv_color_hex(theme::panel),
                                     static_cast<std::uint8_t>(255 * lit)));
    }

    auto phase = [&](float from_ms, float length_ms) {
        return std::clamp((static_cast<float>(elapsed) - from_ms) / length_ms, 0.0f, 1.0f);
    };
    const float        rise = 0.5f - 0.5f * std::cos(phase(300.0f, 5200.0f) * 3.14159265f);
    const std::int32_t top =
        DESK_LOW - static_cast<std::int32_t>(std::lround((DESK_LOW - DESK_HIGH) * rise));
    set_pos_once(s_splash_top, 0, top);

    for (int i = 0; i < SPLASH_ICON_COUNT; ++i) {
        const float t = phase(6000.0f + 1000.0f * static_cast<float>(i), 600.0f);
        const float back =
            1.0f + 2.70158f * std::pow(t - 1.0f, 3.0f) + 1.70158f * std::pow(t - 1.0f, 2.0f);
        const float angle = (-SPLASH_SPREAD + 2.0f * SPLASH_SPREAD * static_cast<float>(i) /
                                                  (SPLASH_ICON_COUNT - 1)) *
                            3.14159265f / 180.0f;
        const float ox    = static_cast<float>(s_splash_origin.x);
        const float oy    = static_cast<float>(s_splash_origin.y + top - DESK_LOW);
        const float to_x  = ox + SPLASH_REACH * std::sin(angle);
        const float to_y  = static_cast<float>(s_splash_origin.y + DESK_HIGH - DESK_LOW) -
                            SPLASH_REACH * std::cos(angle);
        static bool landed[SPLASH_ICON_COUNT] = {};
        if (t <= 0.0f || landed[i]) {
            continue;
        }
        landed[i] = t >= 1.0f;
        set_pos_once(
            s_splash_icon[i],
            static_cast<std::int32_t>(std::lround(ox + (to_x - ox) * back)) - SPLASH_CHIP / 2,
            static_cast<std::int32_t>(std::lround(oy + (to_y - oy) * back)) - SPLASH_CHIP / 2);
        const auto scale = static_cast<std::int32_t>(256 * std::min(t * 2.5f, 1.0f));
        if (lv_obj_get_style_transform_scale_x(s_splash_icon[i], LV_PART_MAIN) != scale) {
            lv_obj_set_style_transform_scale(s_splash_icon[i], scale, 0);
        }
        const auto opa = static_cast<lv_opa_t>(255 * std::min(t * 3.0f, 1.0f));
        if (lv_obj_get_style_opa(s_splash_icon[i], LV_PART_MAIN) != opa) {
            lv_obj_set_style_opa(s_splash_icon[i], opa, 0);
        }
    }

    for (lv_obj_t *leg : s_splash_leg) {
        if (lv_obj_get_y_aligned(leg) != top + DESK_BAR) {
            lv_obj_set_y(leg, top + DESK_BAR);
            lv_obj_set_height(leg, DESK_H - DESK_BAR - 12 - top);
        }
    }
}

// Boot takes nine to ten seconds; the splash always takes twelve, one even
// movement rather than a lurch per step, and waits at the end if boot is slower.
constexpr std::uint32_t SPLASH_GUARD_MS  = 15000;
bool                    s_splash_leaving = false;

// LVGL draws the screen under the top layer even where the splash covers it, and
// Home Assistant filling the pages in cost a fifth of a second a frame.
constexpr int SPLASH_HIDDEN_MAX               = 16;
lv_obj_t     *s_splash_hid[SPLASH_HIDDEN_MAX] = {};

void hide_under_splash()
{
    lv_obj_t *scr  = lv_screen_active();
    int       used = 0;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(scr) && used < SPLASH_HIDDEN_MAX; ++i) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            s_splash_hid[used++] = child;
        }
    }
}

// A cut, not a fade: a full-screen blend takes a quarter of a second a frame here.
void splash_leave()
{
    s_splash_leaving = true;
    for (lv_obj_t *&obj : s_splash_hid) {
        if (obj != nullptr) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            obj = nullptr;
        }
    }
    splash_hide(nullptr);
}

void splash_animate(lv_timer_t *)
{
    const std::uint32_t elapsed = lv_tick_elaps(s_splash_start);
    splash_frame(elapsed);
    if (elapsed >= SPLASH_MS + 400 && s_splash_ready && !s_splash_leaving) {
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
    theme::style_panel(bar, colour, 3);
    lv_obj_set_clickable(bar, false);
    return bar;
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
    hide_under_splash();

    constexpr std::int32_t STEPS_W = 360;
    lv_obj_t              *panel   = lv_obj_create(s_splash);
    lv_obj_set_pos(panel, GAP, GAP);
    lv_obj_set_size(panel, l.screen_w - 2 * GAP, l.screen_h - 2 * GAP);
    theme::style_panel(panel, theme::panel, theme::radius::card);
    lv_obj_set_style_pad_all(panel, PANEL_PAD, 0);
    lv_obj_set_clickable(panel, false);
    const std::int32_t inner_w     = l.screen_w - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t inner_h     = l.screen_h - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t hero_h      = inner_h;
    const std::int32_t hero_full_w = inner_w - STEPS_W - GAP;

    lv_obj_t *hero = theme::make_card(panel);
    lv_obj_set_pos(hero, 0, 0);
    lv_obj_set_size(hero, hero_full_w, hero_h);
    lv_obj_set_clickable(hero, false);

    constexpr std::int32_t SEG_W = 30, SEG_H = 10, SEG_GAP = 8;
    const std::int32_t     hero_w = hero_full_w - 2 * theme::space::l;
    const std::int32_t     arc_h  = static_cast<std::int32_t>(SPLASH_REACH) + SPLASH_CHIP / 2;
    const std::int32_t     block  = arc_h + DESK_H + theme::space::l +
                                    theme::type_display()->line_height +
                                    theme::type_body()->line_height + theme::space::xl + SEG_H;
    const std::int32_t     top    = (hero_h - 2 * theme::space::l - block) / 2 + arc_h;

    // Created before the desk, so they come out from behind it.
    for (int i = 0; i < SPLASH_ICON_COUNT; ++i) {
        s_splash_icon[i] = theme::make_chip(hero, SPLASH_ICONS[i], theme::type_value());
        lv_obj_set_size(s_splash_icon[i], SPLASH_CHIP, SPLASH_CHIP);
        lv_obj_set_style_radius(s_splash_icon[i], SPLASH_CHIP / 2, 0);
        lv_obj_set_style_transform_pivot_x(s_splash_icon[i], SPLASH_CHIP / 2, 0);
        lv_obj_set_style_transform_pivot_y(s_splash_icon[i], SPLASH_CHIP / 2, 0);
        lv_obj_set_style_opa(s_splash_icon[i], LV_OPA_TRANSP, 0);
        lv_obj_t *glyph = lv_obj_get_child(s_splash_icon[i], 0);
        if (SPLASH_IMAGES[i] != nullptr) {
            lv_obj_delete(glyph);
            glyph = lv_image_create(s_splash_icon[i]);
            lv_image_set_src(glyph, SPLASH_IMAGES[i]);
            lv_obj_set_style_image_recolor(glyph, lv_color_hex(theme::primary), 0);
            lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
            lv_obj_center(glyph);
        } else {
            theme::set_text_color(glyph, theme::primary);
            lv_obj_set_style_text_opa(glyph, LV_OPA_COVER, 0);
        }
        lv_obj_set_clickable(s_splash_icon[i], false);
    }
    s_splash_origin = {hero_w / 2, top + DESK_LOW};

    lv_obj_t *desk = lv_obj_create(hero);
    lv_obj_set_size(desk, DESK_W, DESK_H);
    lv_obj_align(desk, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_bg_opa(desk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(desk, 0, 0);
    lv_obj_set_style_pad_all(desk, 0, 0);
    lv_obj_set_clickable(desk, false);
    splash_bar(desk, 8, DESK_H - 12, 88, 12, theme::panel);
    splash_bar(desk, DESK_W - 96, DESK_H - 12, 88, 12, theme::panel);
    s_splash_leg[0] = splash_bar(desk, 44, 0, DESK_BAR, 10, theme::panel);
    s_splash_leg[1] = splash_bar(desk, DESK_W - 44 - DESK_BAR, 0, DESK_BAR, 10, theme::panel);
    s_splash_top    = splash_bar(desk, 0, 0, DESK_W, DESK_BAR, theme::panel);
    lv_obj_set_style_radius(s_splash_top, DESK_BAR / 2, 0);
    theme::fill_accent(s_splash_top);

    const std::int32_t title_y = top + DESK_H + theme::space::l;
    lv_obj_align(theme::make_label(hero, "Smart Flexispot", theme::text, theme::type_display()),
                 LV_ALIGN_TOP_MID, 0, title_y);
    lv_obj_align(
        theme::make_label(hero, "by Wouter ten Brinke", theme::secondary, theme::type_body()),
        LV_ALIGN_TOP_MID, 0, title_y + theme::type_display()->line_height);

    const std::int32_t bar_y    = title_y + theme::type_display()->line_height +
                                  theme::type_body()->line_height + theme::space::xl;
    lv_obj_t          *segments = lv_obj_create(hero);
    lv_obj_set_size(segments, SPLASH_SEGMENTS * (SEG_W + SEG_GAP) - SEG_GAP, SEG_H);
    lv_obj_align(segments, LV_ALIGN_TOP_MID, 0, bar_y);
    lv_obj_set_style_bg_opa(segments, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(segments, 0, 0);
    lv_obj_set_style_pad_all(segments, 0, 0);
    lv_obj_set_clickable(segments, false);
    for (int i = 0; i < SPLASH_SEGMENTS; ++i) {
        s_splash_seg[i] =
            splash_bar(segments, i * (SEG_W + SEG_GAP), 0, SEG_W, SEG_H, theme::panel);
        lv_obj_set_style_radius(s_splash_seg[i], SEG_H / 2, 0);
    }

    char about[80];
    std::snprintf(about, sizeof(about), "M5Stack Tab5, build %s",
                  esp_app_get_description()->version);
    s_splash_about = theme::make_label(hero, about, theme::secondary, theme::type_label());
    lv_obj_set_style_text_opa(s_splash_about, LV_OPA_60, 0);
    lv_obj_align(s_splash_about, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_splash_about, true);
    lv_obj_add_event_cb(
        s_splash,
        [](lv_event_t *) {
            lv_obj_set_hidden(s_splash_about, !lv_obj_has_flag(s_splash_about, LV_OBJ_FLAG_HIDDEN));
        },
        LV_EVENT_CLICKED, nullptr);

    const std::int32_t step_h = (inner_h - (SPLASH_STEP_COUNT - 1) * GAP) / SPLASH_STEP_COUNT;
    const lv_image_dsc_t *const STEP_ICONS[SPLASH_STEP_COUNT] = {&icons::desk_icon,
                                                                 &icons::wifi_icon, nullptr};
    for (int i = 0; i < SPLASH_STEP_COUNT; ++i) {
        lv_obj_t *card = theme::make_card(panel);
        lv_obj_set_pos(card, hero_full_w + GAP, i * (step_h + GAP));
        lv_obj_set_size(card, STEPS_W, step_h);
        lv_obj_set_clickable(card, false);

        lv_obj_t *chip = theme::make_chip(card, STEP_ICONS[i] != nullptr ? "" : LV_SYMBOL_HOME,
                                          fonts::size_28());
        lv_obj_set_clickable(chip, false);
        if (STEP_ICONS[i] != nullptr) {
            lv_obj_t *image = lv_image_create(chip);
            lv_image_set_src(image, STEP_ICONS[i]);
            lv_obj_set_style_image_recolor(image, lv_color_hex(theme::secondary), 0);
            lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
            lv_obj_set_style_image_opa(image, theme::mark_opa, 0);
            lv_obj_center(image);
        }

        lv_obj_t *text = lv_obj_create(card);
        lv_obj_set_size(text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(text, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text, 0, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, theme::space::xs, 0);
        lv_obj_set_clickable(text, false);
        theme::make_label(text, s_splash_steps[i].name, theme::secondary, theme::type_label());
        s_splash_steps[i].state = theme::make_label(text, "", theme::text, theme::type_title());
    }
    splash_mark(0);
    splash_frame(0);
    s_splash_start = lv_tick_get();
    s_splash_tick  = lv_timer_create(splash_animate, 16, nullptr);

    s_splash_guard = lv_timer_create(splash_expired, SPLASH_GUARD_MS, nullptr);
    lv_timer_set_repeat_count(s_splash_guard, 1);
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
