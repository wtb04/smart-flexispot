// The start-up screen: lit first, while the pages are built behind it and the
// radio joins. A desk rises, its pages fly out from behind it, and the steps
// on the right say what is up; it goes with a cut once Home Assistant is.

#include "kit_front.h"

#include "esp_log.h"
#include "kit_fonts.h"
#include "topics.h"
#include "ui_core.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <numbers>

#if defined(ESP_PLATFORM)
#include "esp_app_desc.h"
#endif

namespace ui::kitfront {
namespace {
constexpr char TAG[] = "splash";

using kit::Ink;
using kit::Role;
namespace ic = kit::glyph;

// Driven by the clock alone, so a slow frame does not slow it: twelve seconds
// of movement played in eight, and what is left of it in a moment once
// everything is up.
constexpr std::uint32_t TIMELINE_MS = 12000;
constexpr float         PACE        = 12000.0f / 8000.0f;
constexpr float         RUSH_MS     = 600.0f;
constexpr std::uint32_t HOLD_MS     = 400;
constexpr std::uint32_t FRAME_MS    = 16;
constexpr std::uint32_t GUARD_MS    = 15000;  // gone by then, up or not
constexpr float         RISE_FROM   = 300.0f;
constexpr float         RISE_MS     = 5200.0f;
constexpr float         ICONS_FROM  = 6000.0f;
constexpr float         STAGGER_MS  = 1000.0f;
constexpr float         FLIGHT_MS   = 600.0f;
constexpr float         GROW_RATE   = 2.5f;  // full size by two fifths of its flight
constexpr float         FADE_RATE   = 3.0f;  // opaque by a third
constexpr float         OVERSHOOT   = 1.70158f;

constexpr std::int32_t DESK_W      = 300;
constexpr std::int32_t DESK_H      = 160;
constexpr std::int32_t DESK_BAR    = 16;
constexpr std::int32_t DESK_LOW    = DESK_H - 58;  // the desktop at its lowest, 58 above the floor
constexpr std::int32_t DESK_HIGH   = 6;
constexpr std::int32_t FOOT_W      = 88;
constexpr std::int32_t FOOT_H      = 12;
constexpr std::int32_t FOOT_INSET  = 8;
constexpr std::int32_t LEG_INSET   = 44;
constexpr std::int32_t CHIP        = 56;
constexpr float        REACH       = 158.0f;
constexpr float        SPREAD      = 68.0f;  // degrees either side of straight up
constexpr int          SEGMENTS    = 12;
constexpr std::int32_t SEG_W       = 30;
constexpr std::int32_t SEG_H       = 10;
constexpr std::int32_t SEG_GAP     = 8;
constexpr std::int32_t STEPS_W     = 360;

// The dock's pages, in its order, and the steps of starting.
constexpr const char *PAGE_ICONS[] = {ic::home, ic::calendar_month, ic::flight, ic::timer, ic::settings};
constexpr int         ICON_COUNT   = static_cast<int>(std::size(PAGE_ICONS));
struct Step {
    const char *name;
    const char *glyph;
    lv_obj_t   *state;
};
Step          s_steps[] = {{"Desk", ic::desk, nullptr}, {"Network", ic::wifi, nullptr}, {"Home Assistant", ic::home, nullptr}};
constexpr int STEP_COUNT = static_cast<int>(std::size(s_steps));

lv_obj_t     *s_root = nullptr;
lv_obj_t     *s_top = nullptr, *s_legs[2] = {}, *s_icons[ICON_COUNT] = {}, *s_segments[SEGMENTS] = {};
lv_obj_t     *s_about = nullptr;
lv_point_t    s_origin{};  // the middle of the desktop at its lowest
lv_timer_t   *s_tick = nullptr;
std::uint32_t s_started = 0, s_ticked = 0;
float         s_at   = 0.0f;  // how far into its timeline
float         s_rush = 0.0f;  // its pace once everything is up, if faster
bool          s_ready = false, s_landed[ICON_COUNT] = {};
int           s_steps_shown = -1;

float ease_in_out(float t) { return 0.5f - 0.5f * std::cos(t * std::numbers::pi_v<float>); }

float ease_out_back(float t)
{
    return 1.0f + (OVERSHOOT + 1.0f) * std::pow(t - 1.0f, 3.0f) + OVERSHOOT * std::pow(t - 1.0f, 2.0f);
}

float phase(float at, float from, float length) { return std::clamp((at - from) / length, 0.0f, 1.0f); }

std::int32_t rounded(float v) { return static_cast<std::int32_t>(std::lround(v)); }

// LVGL redraws whatever a style is set on, changed or not: each frame sets only what moved.
void move_once(lv_obj_t *obj, std::int32_t x, std::int32_t y)
{
    if (lv_obj_get_x_aligned(obj) != x || lv_obj_get_y_aligned(obj) != y) {
        lv_obj_set_pos(obj, x, y);
    }
}

void fill_once(lv_obj_t *obj, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), c)) {
        lv_obj_set_style_bg_color(obj, c, 0);
    }
}

lv_obj_t *block(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t colour,
                std::int32_t radius)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(colour), 0);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

lv_obj_t *bare(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

std::int32_t desktop_y(float at)
{
    return DESK_LOW - rounded(static_cast<float>(DESK_LOW - DESK_HIGH) * ease_in_out(phase(at, RISE_FROM, RISE_MS)));
}

void fly(int i, float t, std::int32_t top)
{
    const float back  = ease_out_back(t);
    const float angle = (-SPREAD + 2.0f * SPREAD * static_cast<float>(i) / (ICON_COUNT - 1)) * std::numbers::pi_v<float> / 180.0f;
    const auto  ox    = static_cast<float>(s_origin.x);
    const auto  oy    = static_cast<float>(s_origin.y + top - DESK_LOW);
    const float to_x  = ox + REACH * std::sin(angle);
    const float to_y  = static_cast<float>(s_origin.y + DESK_HIGH - DESK_LOW) - REACH * std::cos(angle);
    move_once(s_icons[i], rounded(ox + (to_x - ox) * back) - CHIP / 2, rounded(oy + (to_y - oy) * back) - CHIP / 2);
    const auto scale = static_cast<std::int32_t>(LV_SCALE_NONE * std::min(t * GROW_RATE, 1.0f));
    if (lv_obj_get_style_transform_scale_x(s_icons[i], LV_PART_MAIN) != scale) {
        lv_obj_set_style_transform_scale(s_icons[i], scale, 0);
    }
    const auto opa = static_cast<lv_opa_t>(LV_OPA_COVER * std::min(t * FADE_RATE, 1.0f));
    if (lv_obj_get_style_opa(s_icons[i], LV_PART_MAIN) != opa) {
        lv_obj_set_style_opa(s_icons[i], opa, 0);
    }
}

void frame(float at)
{
    const kit::Palette &p   = kit::colours();
    const float         run = std::min(at / static_cast<float>(TIMELINE_MS), 1.0f);
    for (int i = 0; i < SEGMENTS; ++i) {
        const float lit = std::clamp(run * SEGMENTS - static_cast<float>(i), 0.0f, 1.0f);
        fill_once(s_segments[i], lv_color_mix(lv_color_hex(p.accent), lv_color_hex(p.surface_hi),
                                              static_cast<std::uint8_t>(LV_OPA_COVER * lit)));
    }
    const std::int32_t top = desktop_y(at);
    move_once(s_top, 0, top);
    for (lv_obj_t *leg : s_legs) {
        if (lv_obj_get_y_aligned(leg) != top + DESK_BAR) {
            lv_obj_set_y(leg, top + DESK_BAR);
            lv_obj_set_height(leg, DESK_H - DESK_BAR - FOOT_H - top);
        }
    }
    for (int i = 0; i < ICON_COUNT; ++i) {
        const float t = phase(at, ICONS_FROM + STAGGER_MS * static_cast<float>(i), FLIGHT_MS);
        if (t > 0.0f && !s_landed[i]) {
            s_landed[i] = t >= 1.0f;
            fly(i, t, top);
        }
    }
}

void mark_steps(int current)
{
    if (current == s_steps_shown) {
        return;
    }
    s_steps_shown = current;
    for (int i = 0; i < STEP_COUNT; ++i) {
        lv_obj_t *state = s_steps[i].state;
        kit::set_words(state, i < current ? "Ready" : i == current ? "Starting" : "Waiting");
        kit::set_ink(state, i < current ? Ink::Accent : i == current ? Ink::Primary : Ink::Tertiary);
    }
}

// A cut, not a fade: a whole screen blended a frame takes a quarter of a second here.
void leave()
{
    ESP_LOGI(TAG, "gone after %u ms", static_cast<unsigned>(lv_tick_elaps(s_started)));
    lv_timer_delete(s_tick);
    s_tick = nullptr;
    lv_obj_delete(s_root);
    s_root = nullptr;
    for (int l = 0; l <= static_cast<int>(kit::Layer::Alert); ++l) {
        lv_obj_set_hidden(kit::layer(static_cast<kit::Layer>(l)), false);
    }
    detail::publish(Topic::Notices);  // the ones that came meanwhile, now there is somewhere to see them
}

void tick(lv_timer_t *)
{
    const bool up = detail::splash_done_wanted() || lv_tick_elaps(s_started) >= GUARD_MS;
    if (up && !s_ready) {
        s_ready = true;
        ESP_LOGI(TAG, "ready after %u ms", static_cast<unsigned>(lv_tick_elaps(s_started)));
    }
    mark_steps(s_ready ? STEP_COUNT : detail::splash_steps_wanted());
    const auto end  = static_cast<float>(TIMELINE_MS + HOLD_MS);
    const auto real = static_cast<float>(lv_tick_elaps(s_ticked));
    s_ticked        = lv_tick_get();
    if (s_ready && s_rush == 0.0f) {
        s_rush = std::max(PACE, (end - s_at) / RUSH_MS);
    }
    s_at = std::min(s_at + real * (s_ready ? s_rush : PACE), s_ready ? end : static_cast<float>(TIMELINE_MS));
    frame(s_at);
    if (s_at >= end) {
        leave();
    }
}

void build_hero(lv_obj_t *hero, std::int32_t w, std::int32_t h)
{
    const kit::Palette &p      = kit::colours();
    const std::int32_t  arc_h  = static_cast<std::int32_t>(REACH) + CHIP / 2;
    const std::int32_t  name_h = lv_font_get_line_height(kit::fonts::h1());
    const std::int32_t  by_h   = lv_font_get_line_height(kit::fonts::body());
    const std::int32_t  height = arc_h + DESK_H + kit::space::xxl + name_h + by_h + kit::space::xxl + SEG_H;
    const std::int32_t  top    = (h - height) / 2 + arc_h;

    // made before the desk, so they come out from behind it
    for (int i = 0; i < ICON_COUNT; ++i) {
        lv_obj_t *chip = block(hero, 0, 0, CHIP, CHIP, p.surface_hi, LV_RADIUS_CIRCLE);
        lv_obj_set_style_transform_pivot_x(chip, CHIP / 2, 0);
        lv_obj_set_style_transform_pivot_y(chip, CHIP / 2, 0);
        lv_obj_set_style_opa(chip, LV_OPA_TRANSP, 0);
        lv_obj_center(kit::icon(chip, PAGE_ICONS[i], 28, false, Ink::Accent));
        s_icons[i] = chip;
    }
    s_origin = {w / 2, top + DESK_LOW};

    lv_obj_t *desk = bare(hero);
    lv_obj_set_size(desk, DESK_W, DESK_H);
    lv_obj_set_pos(desk, (w - DESK_W) / 2, top);
    const std::int32_t foot_y = DESK_H - FOOT_H;
    block(desk, FOOT_INSET, foot_y, FOOT_W, FOOT_H, p.surface_hi, FOOT_H / 2);
    block(desk, DESK_W - FOOT_INSET - FOOT_W, foot_y, FOOT_W, FOOT_H, p.surface_hi, FOOT_H / 2);
    s_legs[0] = block(desk, LEG_INSET, 0, DESK_BAR, 10, p.surface_hi, 0);
    s_legs[1] = block(desk, DESK_W - LEG_INSET - DESK_BAR, 0, DESK_BAR, 10, p.surface_hi, 0);
    s_top     = block(desk, 0, 0, DESK_W, DESK_BAR, p.accent, DESK_BAR / 2);

    const std::int32_t name_y = top + DESK_H + kit::space::xxl;
    lv_obj_t          *name   = kit::text(hero, Role::H1, "Smart Flexispot");
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, name_y);
    lv_obj_align(kit::text(hero, Role::Body, "by Wouter ten Brinke", Ink::Secondary), LV_ALIGN_TOP_MID, 0, name_y + name_h);

    lv_obj_t *row = bare(hero);
    lv_obj_set_size(row, SEGMENTS * (SEG_W + SEG_GAP) - SEG_GAP, SEG_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, name_y + name_h + by_h + kit::space::xxl);
    for (int i = 0; i < SEGMENTS; ++i) {
        s_segments[i] = block(row, i * (SEG_W + SEG_GAP), 0, SEG_W, SEG_H, p.surface_hi, SEG_H / 2);
    }

    char about[64];
#if defined(ESP_PLATFORM)
    std::snprintf(about, sizeof about, "Smart Flexispot, build %s", esp_app_get_description()->version);
#else
    std::snprintf(about, sizeof about, "Smart Flexispot, simulator");
#endif
    s_about = kit::text(hero, Role::Label, about, Ink::Tertiary);
    lv_obj_align(s_about, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_about, true);
}

void build_steps(lv_obj_t *column, std::int32_t h)
{
    const std::int32_t step_h = (h - (STEP_COUNT - 1) * kit::space::l) / STEP_COUNT;
    for (int i = 0; i < STEP_COUNT; ++i) {
        lv_obj_t *tile = kit::tile(column);
        lv_obj_set_pos(tile, 0, i * (step_h + kit::space::l));
        lv_obj_set_size(tile, STEPS_W, step_h);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_pad_all(tile, kit::space::xxl, 0);
        lv_obj_align(kit::icon(tile, s_steps[i].glyph, 34, false, Ink::Secondary), LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *state = kit::text(tile, Role::H3, "Waiting", Ink::Tertiary);
        lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_align_to(kit::text(tile, Role::Label, s_steps[i].name, Ink::Secondary), state, LV_ALIGN_OUT_TOP_LEFT, 0,
                        -kit::space::xs);
        s_steps[i].state = state;
    }
}
}  // namespace

void build_splash()
{
    constexpr std::int32_t MARGIN = kit::space::xxl;
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, kit::size::screen_w, kit::size::screen_h);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(kit::colours().ground), 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);  // nothing under it takes a tap; one shows the build
    lv_obj_add_event_cb(s_root, [](lv_event_t *) { lv_obj_set_hidden(s_about, !lv_obj_is_hidden(s_about)); },
                        LV_EVENT_CLICKED, nullptr);

    const std::int32_t inner_h = kit::size::screen_h - 2 * MARGIN;
    const std::int32_t hero_w  = kit::size::screen_w - 3 * MARGIN - STEPS_W;
    lv_obj_t          *hero    = bare(s_root);
    lv_obj_set_pos(hero, MARGIN, MARGIN);
    lv_obj_set_size(hero, hero_w, inner_h);
    build_hero(hero, hero_w, inner_h);
    lv_obj_t *column = bare(s_root);
    lv_obj_set_pos(column, 2 * MARGIN + hero_w, MARGIN);
    lv_obj_set_size(column, STEPS_W, inner_h);
    build_steps(column, inner_h);

    // LVGL draws what is under the top layer even where it is covered, and the
    // pages being built and filled behind would cost every frame of it
    for (int l = 0; l <= static_cast<int>(kit::Layer::Alert); ++l) {
        lv_obj_set_hidden(kit::layer(static_cast<kit::Layer>(l)), true);
    }
    mark_steps(0);
    frame(0.0f);
    s_started = s_ticked = lv_tick_get();
    s_tick               = lv_timer_create(tick, FRAME_MS, nullptr);
}

bool splash_showing() { return s_root != nullptr; }
}  // namespace ui::kitfront

namespace ui::detail {
bool splash_showing() { return kitfront::splash_showing(); }
}  // namespace ui::detail
