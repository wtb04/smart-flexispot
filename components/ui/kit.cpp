#include "kit.h"

#include "icons.h"

#include "kit_bake.h"
#include "kit_fonts.h"
#include "kit_pixels.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ui::kit {
void restyle_lists();  // kit_lists.cpp

namespace {

// The darks sit on RGB565's grid (red and blue in eights, green in fours): off
// it, the panel rounds a warm brown to olive, as it loses more of the red and
// blue than of the green.
constexpr Palette DARK{
    .ground = 0x100c08, .surface = 0x201c18, .surface_hi = 0x282420,
    .raised = 0x302820, .raised_hi = 0x383028, .fill_opa = 20,
    .ink = 0xf6efe7, .ink2_opa = 158, .ink3_opa = 92, .hair_opa = 20, .tick_opa = 72,
    .hair = 0xffffff,
    .accent = 0xff8a3d, .on_accent = 0x1f0e03,
    .green = 0x86d27c, .amber = 0xffbe55, .red = 0xff6b5e, .lamp = 0xffd36b, .on_lamp = 0x2e1f00,
    .blue = 0x6fb6ff,
    .shadow = 0x000000, .shadow_opa = 110, .edge_opa = 120,
    .glass = 0x484038, .glass_clear_opa = 56, .glass_solid_opa = 214, .rim_opa = 38,
    .glass_edge_opa = 115, .glass_shadow_opa = 170,
    .scrim = 0x080400, .scrim_opa = 107,
};

constexpr Palette LIGHT{
    .ground = 0xe9e3db, .surface = 0xfbf8f4, .surface_hi = 0xfffbf5,
    .raised = 0xf1ebe4, .raised_hi = 0xe8e0d7, .fill_opa = 18,
    .ink = 0x1d1813, .ink2_opa = 158, .ink3_opa = 97, .hair_opa = 20, .tick_opa = 77,
    .hair = 0x1d1813,
    .accent = 0xf2711c, .on_accent = 0xffffff,
    .green = 0x3f9f45, .amber = 0xd68a00, .red = 0xe0483b, .lamp = 0xf4b400, .on_lamp = 0x2e1f00,
    .blue = 0x2f86e0,
    .shadow = 0x50371e, .shadow_opa = 46, .edge_opa = 22,
    .glass = 0xfcf9f5, .glass_clear_opa = 87, .glass_solid_opa = 224, .rim_opa = 230,
    .glass_edge_opa = 31, .glass_shadow_opa = 85,
    .scrim = 0xe9e3db, .scrim_opa = 128,
};

struct Glow {
    float x, y, rx, ry, reach;  // centre and radii as fractions of the screen; reach as CSS's stop
    std::uint32_t colour;
    float alpha;
};
// The lamp-lit corners of the ground, as the mockups have them.
constexpr Glow DARK_GLOWS[] = {
    {0.00f, 0.00f, 0.70f, 0.90f, 0.60f, 0xff8a3d, 0.14f},
    {0.03f, 0.10f, 0.30f, 0.48f, 0.72f, 0xff8034, 0.42f},
    {0.80f, 0.00f, 0.28f, 0.42f, 0.72f, 0xff603c, 0.22f},
    {0.06f, 1.00f, 0.40f, 0.55f, 0.72f, 0xffb260, 0.20f},
};
constexpr Glow LIGHT_GLOWS[] = {
    {0.00f, 0.00f, 0.70f, 0.90f, 0.60f, 0xffa05a, 0.26f},
    {0.03f, 0.10f, 0.30f, 0.48f, 0.72f, 0xff9650, 0.55f},
    {0.80f, 0.00f, 0.28f, 0.42f, 0.72f, 0xff826e, 0.32f},
    {0.06f, 1.00f, 0.40f, 0.55f, 0.72f, 0xffc478, 0.40f},
};

Look          s_look   = Look::Dark;
bool          s_blur = true, s_shadows = true;
int           s_glass  = 50;
std::uint32_t s_accent = 0;  // 0: the look's own
Palette       s_pal    = DARK;

lv_style_t s_ground, s_tile, s_tile_hi, s_well;
lv_style_t s_glass_sm, s_glass_lg, s_glass_clear;
lv_style_t s_role[11];
lv_style_t s_ink[8], s_dot[8], s_mark;
lv_style_t s_btn_open;
lv_style_t s_btn, s_btn_pressed, s_btn_accent, s_btn_flat, s_btn_soft, s_btn_round, s_btn_checked;
// what each kind turns to under the finger: a darker fill of its own colour
lv_style_t s_btn_flat_pressed, s_btn_accent_pressed, s_btn_checked_pressed, s_lamp_on_pressed;
lv_style_t s_lamp, s_lamp_on, s_lamp_on_ink;
lv_style_t s_hair;
lv_style_t s_switch, s_switch_on, s_switch_knob;
lv_style_t s_seg, s_seg_item, s_seg_thumb;

lv_style_transition_dsc_t s_press_tr, s_release_tr;
constexpr lv_style_prop_t PRESS_PROPS[] = {LV_STYLE_TRANSFORM_SCALE_X, LV_STYLE_TRANSFORM_SCALE_Y,
                                           LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_SHADOW_OPA,
                                           LV_STYLE_PROP_INV};

struct Restyler {
    lv_obj_t *owner;
    void (*cb)(void *);
    void *ctx;
};
Restyler s_restylers[64];
int      s_restyler_count = 0;

lv_draw_buf_t *s_ground_buf = nullptr;
lv_obj_t      *s_ground_img = nullptr;

lv_color_t hex(std::uint32_t c) { return lv_color_hex(c); }

std::uint32_t mix(std::uint32_t a, std::uint32_t b, int b_percent)
{
    const auto ch = [&](int shift) {
        const int x = static_cast<int>((a >> shift) & 0xff), y = static_cast<int>((b >> shift) & 0xff);
        return static_cast<std::uint32_t>(x + (y - x) * b_percent / 100) << shift;
    };
    return ch(16) | ch(8) | ch(0);
}

const lv_font_t *font_of(Role role)
{
    switch (role) {
    case Role::Label: return fonts::label();
    case Role::Small: return fonts::small();
    case Role::Body: return fonts::body();
    case Role::H3: return fonts::h3();
    case Role::H2: return fonts::h2();
    case Role::H1: return fonts::h1();
    case Role::ReadS: return fonts::read_s();
    case Role::ReadM: return fonts::read_m();
    case Role::ReadL: return fonts::read_l();
    case Role::ReadXL: return fonts::read_xl();
    case Role::ReadXXL: return fonts::read_xxl();
    }
    return fonts::body();
}

void set_shadow(lv_style_t *st, std::int32_t width, std::int32_t y, std::int32_t spread, lv_opa_t opa)
{
    lv_style_set_shadow_width(st, s_shadows ? width : 0);
    lv_style_set_shadow_offset_y(st, y);
    lv_style_set_shadow_spread(st, spread);
    lv_style_set_shadow_color(st, hex(s_pal.shadow));
    lv_style_set_shadow_opa(st, opa);
}

void style_glass(lv_style_t *st, std::int32_t blur, std::int32_t shadow_w, std::int32_t shadow_y,
                 lv_opa_t opa)
{
    // a flat tint: a sheen this soft and this tall bands on the panel, which LVGL does not dither
    lv_style_set_bg_color(st, hex(s_pal.glass));
    lv_style_set_bg_grad_dir(st, LV_GRAD_DIR_NONE);
    lv_style_set_bg_opa(st, opa);
    // no live blur in the shared glass: the chrome sits over the ground, which is
    // soft already, and LVGL re-blurs a whole widget whenever anything near it
    // redraws, which the panel cannot afford; popovers bring their own
    (void)blur;
    lv_style_set_blur_backdrop(st, false);
    lv_style_set_blur_radius(st, 0);
    lv_style_set_border_width(st, 1);
    lv_style_set_border_color(st, hex(0xffffff));
    lv_style_set_border_opa(st, s_pal.rim_opa);
    lv_style_set_outline_width(st, 1);
    lv_style_set_outline_pad(st, 0);
    lv_style_set_outline_color(st, hex(s_pal.shadow));
    lv_style_set_outline_opa(st, s_pal.glass_edge_opa);
    set_shadow(st, shadow_w, shadow_y, -8, s_pal.glass_shadow_opa);
    lv_style_set_text_color(st, hex(s_pal.ink));
}

lv_opa_t glass_opa()
{
    return static_cast<lv_opa_t>(s_pal.glass_clear_opa +
                                 (s_pal.glass_solid_opa - s_pal.glass_clear_opa) * s_glass / 100);
}

void apply()
{
    s_pal = s_look == Look::Dark ? DARK : LIGHT;
    if (s_accent != 0) {
        s_pal.accent = s_accent;
    }

    lv_style_set_bg_color(&s_ground, hex(s_pal.ground));
    lv_style_set_bg_opa(&s_ground, LV_OPA_COVER);
    lv_style_set_text_color(&s_ground, hex(s_pal.ink));

    for (lv_style_t *st : {&s_tile, &s_tile_hi}) {
        lv_style_set_bg_color(st, hex(st == &s_tile ? s_pal.surface : s_pal.surface_hi));
        lv_style_set_bg_opa(st, LV_OPA_COVER);
        lv_style_set_radius(st, radius::tile);
        lv_style_set_border_width(st, 0);
        lv_style_set_outline_width(st, 1);
        lv_style_set_outline_pad(st, 0);
        lv_style_set_outline_color(st, hex(s_pal.shadow));
        lv_style_set_outline_opa(st, s_pal.edge_opa);
        lv_style_set_pad_all(st, space::xl);
        lv_style_set_text_color(st, hex(s_pal.ink));
    }
    // under the tile rather than round it: tight, pushed down, and pulled in at the sides
    set_shadow(&s_tile, 26, 10, -10, s_pal.shadow_opa);
    set_shadow(&s_tile_hi, 40, 16, -12, s_pal.shadow_opa);

    // flat, neither sunk in nor lifted
    lv_style_set_bg_color(&s_well, hex(s_pal.ink));
    lv_style_set_bg_opa(&s_well, s_pal.fill_opa);
    lv_style_set_radius(&s_well, radius::inner);
    lv_style_set_border_width(&s_well, 0);
    lv_style_set_pad_all(&s_well, space::m);

    const lv_opa_t g = glass_opa();
    style_glass(&s_glass_sm, 20, 44, 16, g);
    // big glass reads more opaque, so what is on it stays legible over anything
    style_glass(&s_glass_lg, 40, 80, 28, static_cast<lv_opa_t>(g + (s_pal.glass_solid_opa - g) * 45 / 100));
    style_glass(&s_glass_clear, 10, 24, 8, static_cast<lv_opa_t>(g * 2 / 3));
    lv_style_set_bg_color(&s_glass_clear, hex(0x14100d));
    lv_style_set_bg_grad_color(&s_glass_clear, hex(0x14100d));
    lv_style_set_text_color(&s_glass_clear, hex(0xffffff));

    for (int i = 0; i < 11; ++i) {
        lv_style_set_text_font(&s_role[i], font_of(static_cast<Role>(i)));
    }
    lv_style_set_text_letter_space(&s_role[static_cast<int>(Role::Label)], 1);

    const std::uint32_t inks[8] = {s_pal.ink, s_pal.ink, s_pal.ink, s_pal.accent,
                                   s_pal.on_accent, s_pal.green, s_pal.amber, s_pal.red};
    const lv_opa_t ink_opa[8] = {LV_OPA_COVER, s_pal.ink2_opa, s_pal.ink3_opa, LV_OPA_COVER,
                                 LV_OPA_COVER, LV_OPA_COVER, LV_OPA_COVER, LV_OPA_COVER};
    for (int i = 0; i < 8; ++i) {
        lv_style_set_text_color(&s_ink[i], hex(inks[i]));
        lv_style_set_text_opa(&s_ink[i], ink_opa[i]);
        lv_style_set_bg_color(&s_dot[i], hex(inks[i]));
        lv_style_set_bg_opa(&s_dot[i], ink_opa[i]);
    }

    lv_style_set_bg_color(&s_btn, hex(s_pal.raised));
    lv_style_set_bg_opa(&s_btn, LV_OPA_COVER);
    lv_style_set_radius(&s_btn, radius::button);
    lv_style_set_border_width(&s_btn, 0);
    lv_style_set_text_color(&s_btn, hex(s_pal.ink));
    lv_style_set_pad_hor(&s_btn, space::xl);
    lv_style_set_pad_ver(&s_btn, 0);
    lv_style_set_pad_column(&s_btn, space::s);
    lv_style_set_min_height(&s_btn, size::button);
    lv_style_set_transform_pivot_x(&s_btn, lv_pct(50));
    lv_style_set_transform_pivot_y(&s_btn, lv_pct(50));
    lv_style_set_transition(&s_btn, &s_release_tr);
    set_shadow(&s_btn, 10, 4, -3, s_look == Look::Dark ? 120 : 44);

    lv_style_set_transform_scale_x(&s_btn_pressed, motion::press_scale);
    lv_style_set_transform_scale_y(&s_btn_pressed, motion::press_scale);
    lv_style_set_bg_color(&s_btn_pressed, hex(s_pal.raised_hi));
    lv_style_set_transition(&s_btn_pressed, &s_press_tr);

    lv_style_set_bg_color(&s_btn_accent, hex(s_pal.accent));
    lv_style_set_text_color(&s_btn_accent, hex(s_pal.on_accent));
    lv_style_set_shadow_color(&s_btn_accent, hex(s_pal.accent));
    lv_style_set_shadow_opa(&s_btn_accent, 110);
    lv_style_set_shadow_width(&s_btn_accent, s_shadows ? 22 : 0);
    lv_style_set_shadow_offset_y(&s_btn_accent, 6);

    lv_style_set_bg_color(&s_btn_open, hex(s_pal.raised_hi));
    lv_style_set_bg_opa(&s_btn_open, LV_OPA_COVER);

    lv_style_set_bg_opa(&s_btn_flat, LV_OPA_TRANSP);
    lv_style_set_shadow_opa(&s_btn_flat, LV_OPA_TRANSP);
    // a bare glyph has no fill to darken: a faint one comes up under the finger
    lv_style_set_bg_color(&s_btn_flat_pressed, hex(s_pal.hair));
    lv_style_set_bg_opa(&s_btn_flat_pressed, static_cast<lv_opa_t>(s_pal.hair_opa * 2));
    lv_style_set_bg_color(&s_btn_accent_pressed, hex(mix(s_pal.accent, 0x000000, 16)));
    lv_style_set_bg_color(&s_btn_checked_pressed, hex(mix(s_pal.accent, 0x000000, 16)));
    lv_style_set_bg_opa(&s_btn_checked_pressed, LV_OPA_COVER);
    lv_style_set_bg_color(&s_lamp_on_pressed, hex(mix(s_pal.lamp, 0x000000, 14)));

    lv_style_set_bg_color(&s_btn_soft, hex(s_pal.accent));
    lv_style_set_bg_opa(&s_btn_soft, 40);
    lv_style_set_text_color(&s_btn_soft, hex(s_pal.accent));
    lv_style_set_shadow_opa(&s_btn_soft, LV_OPA_TRANSP);

    lv_style_set_radius(&s_btn_round, radius::round);
    lv_style_set_pad_hor(&s_btn_round, 0);

    lv_style_set_bg_color(&s_btn_checked, hex(s_pal.accent));
    lv_style_set_bg_opa(&s_btn_checked, LV_OPA_COVER);
    lv_style_set_text_color(&s_btn_checked, hex(s_pal.on_accent));
    lv_style_set_text_opa(&s_btn_checked, LV_OPA_COVER);

    lv_style_set_radius(&s_lamp, radius::inner);
    lv_style_set_pad_all(&s_lamp, space::l);
    lv_style_set_bg_color(&s_lamp_on, hex(s_pal.lamp));
    lv_style_set_text_color(&s_lamp_on, hex(s_pal.on_lamp));
    lv_style_set_shadow_color(&s_lamp_on, hex(s_pal.lamp));
    lv_style_set_shadow_opa(&s_lamp_on, 120);
    lv_style_set_shadow_width(&s_lamp_on, s_shadows ? 26 : 0);
    lv_style_set_shadow_offset_y(&s_lamp_on, 6);
    lv_style_set_text_color(&s_lamp_on_ink, hex(s_pal.on_lamp));
    lv_style_set_text_opa(&s_lamp_on_ink, 200);

    lv_style_set_image_recolor(&s_mark, hex(s_pal.ink));
    lv_style_set_image_recolor_opa(&s_mark, LV_OPA_COVER);

    lv_style_set_bg_color(&s_hair, hex(s_pal.hair));
    lv_style_set_bg_opa(&s_hair, s_pal.hair_opa);
    lv_style_set_radius(&s_hair, 0);
    lv_style_set_border_width(&s_hair, 0);

    lv_style_set_bg_color(&s_switch, hex(s_pal.ink));
    lv_style_set_bg_opa(&s_switch, static_cast<lv_opa_t>(s_pal.fill_opa * 2));
    lv_style_set_bg_color(&s_switch_on, hex(s_pal.accent));
    lv_style_set_bg_opa(&s_switch_on, LV_OPA_COVER);
    lv_style_set_bg_color(&s_switch_knob, hex(0xffffff));
    lv_style_set_bg_opa(&s_switch_knob, LV_OPA_COVER);
    lv_style_set_pad_all(&s_switch_knob, -4);
    set_shadow(&s_switch_knob, 8, 2, 0, 90);

    lv_style_set_bg_color(&s_seg, hex(s_pal.ink));
    lv_style_set_bg_opa(&s_seg, s_pal.fill_opa);
    lv_style_set_radius(&s_seg, 18);
    lv_style_set_border_width(&s_seg, 0);
    lv_style_set_pad_all(&s_seg, 5);
    lv_style_set_bg_opa(&s_seg_item, LV_OPA_TRANSP);
    lv_style_set_text_color(&s_seg_item, hex(s_pal.ink));
    lv_style_set_text_opa(&s_seg_item, s_pal.ink2_opa);
    lv_style_set_bg_color(&s_seg_thumb, hex(s_look == Look::Dark ? s_pal.raised_hi : s_pal.surface_hi));
    lv_style_set_bg_opa(&s_seg_thumb, LV_OPA_COVER);
    lv_style_set_radius(&s_seg_thumb, 13);
    set_shadow(&s_seg_thumb, 10, 2, -2, s_look == Look::Dark ? 140 : 50);

    restyle_lists();
    lv_obj_report_style_change(nullptr);
    for (int i = 0; i < s_restyler_count; ++i) {
        s_restylers[i].cb(s_restylers[i].ctx);
    }
    bake::restyle();
}

void init_styles()
{
    for (lv_style_t *st : {&s_ground, &s_tile, &s_tile_hi, &s_well, &s_glass_sm, &s_glass_lg, &s_glass_clear,
                           &s_btn, &s_btn_open, &s_btn_pressed, &s_btn_accent, &s_btn_flat, &s_btn_soft, &s_btn_round,
                           &s_btn_flat_pressed, &s_btn_accent_pressed, &s_btn_checked_pressed, &s_lamp_on_pressed,
                           &s_btn_checked, &s_lamp, &s_lamp_on, &s_lamp_on_ink, &s_hair, &s_switch,
                           &s_switch_on, &s_switch_knob, &s_seg, &s_seg_item, &s_seg_thumb}) {
        lv_style_init(st);
    }
    for (lv_style_t &st : s_role) {
        lv_style_init(&st);
    }
    for (lv_style_t &st : s_ink) {
        lv_style_init(&st);
    }
    for (lv_style_t &st : s_dot) {
        lv_style_init(&st);
    }
    lv_style_init(&s_mark);
    lv_style_transition_dsc_init(&s_press_tr, PRESS_PROPS, lv_anim_path_linear, motion::press_ms, 0, nullptr);
    lv_style_transition_dsc_init(&s_release_tr, PRESS_PROPS, motion::spring, motion::release_ms, 0, nullptr);
}

// The ground at an eighth of the screen, worked out from the glows, then
// stretched to the screen with a little noise: the noise is the grain, and it
// keeps the soft gradients from banding in 16-bit colour.
void paint_ground()
{
    constexpr int W = size::screen_w, H = size::screen_h, SW = W / 8 + 1, SH = H / 8 + 1;
    // in PSRAM and only while painting: it is far too big for internal RAM
    auto *small = static_cast<float (*)[SW][3]>(lv_malloc(sizeof(float) * SH * SW * 3));
    if (small == nullptr) {
        return;
    }
    const Glow *glows = s_look == Look::Dark ? DARK_GLOWS : LIGHT_GLOWS;
    const float base[3] = {static_cast<float>((s_pal.ground >> 16) & 0xff), static_cast<float>((s_pal.ground >> 8) & 0xff),
                           static_cast<float>(s_pal.ground & 0xff)};
    for (int y = 0; y < SH; ++y) {
        for (int x = 0; x < SW; ++x) {
            float px[3] = {base[0], base[1], base[2]};
            for (int g = 0; g < 4; ++g) {
                const Glow &glow = glows[g];
                const float dx = (x * 8.0f / W - glow.x) / glow.rx, dy = (y * 8.0f / H - glow.y) / glow.ry;
                float f = 1.0f - std::sqrt(dx * dx + dy * dy) / glow.reach;
                if (f <= 0.0f) {
                    continue;
                }
                f = f * f * (3.0f - 2.0f * f) * glow.alpha;
                const float c[3] = {static_cast<float>((glow.colour >> 16) & 0xff),
                                    static_cast<float>((glow.colour >> 8) & 0xff), static_cast<float>(glow.colour & 0xff)};
                for (int k = 0; k < 3; ++k) {
                    px[k] += (c[k] - px[k]) * f;
                }
            }
            for (int k = 0; k < 3; ++k) {
                small[y][x][k] = px[k];
            }
        }
    }
    if (s_ground_buf == nullptr) {
        s_ground_buf = lv_draw_buf_create(W, H, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    }
    std::uint32_t seed = 0x9e3779b9u;
    for (int y = 0; y < H; ++y) {
        auto *row = reinterpret_cast<std::uint16_t *>(s_ground_buf->data + y * s_ground_buf->header.stride);
        const float fy = y / 8.0f;
        const int   y0 = static_cast<int>(fy), y1 = std::min(y0 + 1, SH - 1);
        const float ty = fy - y0;
        for (int x = 0; x < W; ++x) {
            const float fx = x / 8.0f;
            const int   x0 = static_cast<int>(fx), x1 = std::min(x0 + 1, SW - 1);
            const float tx = fx - x0;
            seed = seed * 1664525u + 1013904223u;
            const float noise = static_cast<float>((seed >> 24) & 0xff) / 255.0f * 4.0f - 2.0f;  // the grain
            int out[3];
            for (int k = 0; k < 3; ++k) {
                const float top = small[y0][x0][k] + (small[y0][x1][k] - small[y0][x0][k]) * tx;
                const float bot = small[y1][x0][k] + (small[y1][x1][k] - small[y1][x0][k]) * tx;
                out[k] = std::clamp(static_cast<int>(top + (bot - top) * ty + noise), 0, 255);
            }
            row[x] = pixels::dither(out[0], out[1], out[2], x, y);
        }
    }
    lv_free(small);
    lv_draw_buf_flush_cache(s_ground_buf, nullptr);
    bake::painted();
    if (s_ground_img != nullptr) {
        lv_obj_invalidate(s_ground_img);
    }
}

float spring_at(float t, float zeta, float omega)
{
    if (zeta >= 1.0f) {
        return 1.0f - (1.0f + omega * t) * std::exp(-omega * t);
    }
    const float wd = omega * std::sqrt(1.0f - zeta * zeta);
    return 1.0f - std::exp(-zeta * omega * t) * (std::cos(wd * t) + zeta * omega / wd * std::sin(wd * t));
}

int32_t run_spring(const lv_anim_t *a, float zeta)
{
    if (a->act_time >= a->duration) {
        return a->end_value;
    }
    const float t = std::max(0.0f, static_cast<float>(a->act_time) / static_cast<float>(a->duration));
    const float x = spring_at(t, zeta, 8.0f);
    return a->start_value + static_cast<int32_t>(std::lround(static_cast<float>(a->end_value - a->start_value) * x));
}

void on_seg_click(lv_event_t *e);

}  // namespace

namespace motion {
int32_t spring(const lv_anim_t *a) { return run_spring(a, 0.86f); }
int32_t settle(const lv_anim_t *a) { return run_spring(a, 1.0f); }
}  // namespace motion

void init(Look look)
{
    s_look = look;
    init_styles();
    apply();
}

// Each of these restyles every screen and redraws all of it, so only when
// something changes.
void set_look(Look look)
{
    if (look == s_look) {
        return;
    }
    s_look = look;
    apply();
    if (s_ground_img != nullptr) {
        paint_ground();
    }
}

void set_glass(int percent)
{
    percent = std::clamp(percent, 0, 100);
    if (percent == s_glass) {
        return;
    }
    s_glass = percent;
    apply();
}

void set_effects(bool blur, bool shadows)
{
    if (blur == s_blur && shadows == s_shadows) {
        return;
    }
    s_blur    = blur;
    s_shadows = shadows;
    apply();
}

void set_accent(std::uint32_t colour)
{
    if (colour == s_accent) {
        return;
    }
    s_accent = colour;
    apply();
}

Look look() { return s_look; }
int glass() { return s_glass; }
const Palette &colours() { return s_pal; }

void on_restyle(lv_obj_t *owner, void (*cb)(void *), void *ctx)
{
    if (s_restyler_count == static_cast<int>(std::size(s_restylers))) {
        return;
    }
    s_restylers[s_restyler_count++] = {owner, cb, ctx};
    lv_obj_add_event_cb(
        owner,
        [](lv_event_t *e) {
            lv_obj_t *gone = lv_event_get_target_obj(e);
            int       kept = 0;
            for (int i = 0; i < s_restyler_count; ++i) {
                if (s_restylers[i].owner != gone) {
                    s_restylers[kept++] = s_restylers[i];
                }
            }
            s_restyler_count = kept;
        },
        LV_EVENT_DELETE, nullptr);
}

lv_color_t colour(Ink ink)
{
    switch (ink) {
    case Ink::Accent: return hex(s_pal.accent);
    case Ink::OnAccent: return hex(s_pal.on_accent);
    case Ink::Green: return hex(s_pal.green);
    case Ink::Amber: return hex(s_pal.amber);
    case Ink::Red: return hex(s_pal.red);
    default: return hex(s_pal.ink);
    }
}

lv_obj_t *ground(lv_obj_t *parent)
{
    lv_obj_add_style(lv_obj_get_screen(parent), &s_ground, 0);
    paint_ground();
    // the picture as the parent's own opaque background rather than a child:
    // LVGL only skips drawing what lies under an opaque thing it can reach
    // through parents that are opaque themselves
    s_ground_img = parent;
    lv_obj_set_style_bg_image_src(parent, s_ground_buf, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(parent, hex(s_pal.ground), 0);
    return parent;
}

namespace {
lv_obj_t *s_layers[5]{};
lv_obj_t *s_edge = nullptr;

// Apple's scroll edge: a band under the bar that fades in as content slides
// beneath it, so the bar reads as floating over what scrolls rather than
// cutting it off.
void restyle_edge(void *)
{
    if (s_edge == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(s_edge, hex(s_pal.ground), 0);
    lv_obj_set_style_bg_grad_color(s_edge, hex(s_pal.ground), 0);
}

void on_content_scroll(lv_event_t *e)
{
    const std::int32_t y = lv_obj_get_scroll_y(lv_event_get_target_obj(e));
    lv_obj_set_style_opa(s_edge, static_cast<lv_opa_t>(std::clamp<std::int32_t>(y * 6, 0, 255)), 0);
}
}  // namespace

void screen(lv_obj_t *scr)
{
    lv_obj_remove_style_all(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *l = lv_obj_create(scr);
        lv_obj_remove_style_all(l);
        lv_obj_set_size(l, size::screen_w, size::screen_h);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        s_layers[i] = l;
    }
    ground(s_layers[static_cast<int>(Layer::Ground)]);
    lv_obj_t *content = s_layers[static_cast<int>(Layer::Content)];
    bake::begin(s_layers[static_cast<int>(Layer::Ground)], content, s_ground_buf);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(content, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_bottom(content, space::l, 0);
    lv_obj_add_event_cb(content, on_content_scroll, LV_EVENT_SCROLL, nullptr);

    s_edge = lv_obj_create(s_layers[static_cast<int>(Layer::Chrome)]);
    lv_obj_remove_style_all(s_edge);
    lv_obj_set_size(s_edge, size::screen_w, 132);
    lv_obj_set_style_bg_opa(s_edge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_main_opa(s_edge, 235, 0);
    lv_obj_set_style_bg_grad_opa(s_edge, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_dir(s_edge, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(s_edge, 90, 0);
    lv_obj_set_style_opa(s_edge, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_edge, LV_OBJ_FLAG_CLICKABLE);
    restyle_edge(nullptr);
    on_restyle(s_edge, restyle_edge, nullptr);
}

lv_obj_t *layer(Layer which)
{
    lv_obj_t *l = s_layers[static_cast<int>(which)];
    return l != nullptr ? l : lv_screen_active();
}

lv_obj_t *tile(lv_obj_t *parent, Lift lift)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, lift == Lift::Low ? &s_tile : &s_tile_hi, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    bake::cast(obj);
    return obj;
}

lv_obj_t *well(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &s_well, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *glass(lv_obj_t *parent, Glass kind)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    const lv_style_t *st = kind == Glass::Small ? &s_glass_sm : kind == Glass::Large ? &s_glass_lg : &s_glass_clear;
    lv_obj_add_style(obj, st, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    bake::glass(obj, st);
    return obj;
}

lv_obj_t *text(lv_obj_t *parent, Role role, const char *value, Ink ink)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_add_style(label, &s_role[static_cast<int>(role)], 0);
    lv_obj_add_style(label, &s_ink[static_cast<int>(ink)], 0);
    lv_label_set_text(label, value);
    return label;
}

void set_ink(lv_obj_t *label, Ink ink)
{
    for (lv_style_t &st : s_ink) {
        lv_obj_remove_style(label, &st, 0);
    }
    lv_obj_add_style(label, &s_ink[static_cast<int>(ink)], 0);
}

lv_obj_t *icon(lv_obj_t *parent, const char *glyph, std::int32_t px, bool filled, Ink ink)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, fonts::icon(px, filled), 0);
    lv_obj_add_style(label, &s_ink[static_cast<int>(ink)], 0);
    lv_label_set_text(label, glyph);
    lv_obj_set_user_data(label, reinterpret_cast<void *>(static_cast<std::intptr_t>(px)));
    return label;
}

// Setting a font or a text redraws the label even when nothing changes, and
// pages set these on every update of what they show.
void set_icon(lv_obj_t *label, const char *glyph, bool filled)
{
    const auto       px   = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(label)));
    const lv_font_t *font = fonts::icon(px > 0 ? px : 28, filled);
    if (lv_obj_get_style_text_font(label, LV_PART_MAIN) != font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    if (std::strcmp(lv_label_get_text(label), glyph) != 0) {
        lv_label_set_text(label, glyph);
    }
}

lv_obj_t *hairline(lv_obj_t *parent, bool vertical, std::int32_t length)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &s_hair, 0);
    lv_obj_set_size(obj, vertical ? 1 : length, vertical ? length : 1);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *dot(lv_obj_t *parent, Ink ink, std::int32_t px)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, px, px);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(obj, &s_dot[static_cast<int>(ink)], 0);
    lv_obj_set_user_data(obj, reinterpret_cast<void *>(static_cast<std::intptr_t>(ink)));
    return obj;
}

void set_dot(lv_obj_t *obj, Ink ink)
{
    const auto was = static_cast<Ink>(reinterpret_cast<std::intptr_t>(lv_obj_get_user_data(obj)));
    if (was == ink) {
        return;
    }
    lv_obj_remove_style(obj, &s_dot[static_cast<int>(was)], 0);
    lv_obj_add_style(obj, &s_dot[static_cast<int>(ink)], 0);
    lv_obj_set_user_data(obj, reinterpret_cast<void *>(static_cast<std::intptr_t>(ink)));
}

Status status(lv_obj_t *parent)
{
    Status s;
    s.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s.root);
    lv_obj_set_size(s.root, LV_SIZE_CONTENT, size::bar);
    lv_obj_set_flex_flow(s.root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s.root, space::m, 0);
    lv_obj_set_style_pad_hor(s.root, space::m, 0);
    lv_obj_remove_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s.root, LV_OBJ_FLAG_CLICKABLE);
    // Montserrat, as the clock always was; its figures differ in width, so the
    // clock is as wide as its widest time and the marks beside it stay put
    const lv_font_t *font = &lv_font_montserrat_28;
    s.clock = lv_label_create(s.root);
    lv_obj_set_style_text_font(s.clock, font, 0);
    lv_obj_add_style(s.clock, &s_ink[static_cast<int>(Ink::Primary)], 0);
    std::int32_t widest = 0;
    for (char d = '0'; d <= '9'; ++d) {
        const char t[] = {d, d, ':', d, d, '\0'};
        lv_point_t size{};
        lv_text_get_size(&size, t, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        widest = std::max(widest, size.x);
    }
    lv_obj_set_width(s.clock, widest);
    lv_obj_set_style_text_align(s.clock, LV_TEXT_ALIGN_RIGHT, 0);  // close to the marks, as the minutes go by
    lv_label_set_text(s.clock, "");
    for (lv_obj_t **mark : {&s.wifi, &s.phone}) {
        *mark = lv_image_create(s.root);
        lv_obj_add_style(*mark, &s_mark, 0);
        lv_obj_remove_flag(*mark, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_image_set_src(s.wifi, &icons::wifi_off_icon);
    lv_image_set_src(s.phone, &icons::phone_off_icon);
    return s;
}

void set_status(const Status &s, const char *time, bool wifi, bool phone)
{
    if (std::strcmp(lv_label_get_text(s.clock), time) != 0) {
        lv_label_set_text(s.clock, time);
    }
    const void *w = wifi ? &icons::wifi_icon : &icons::wifi_off_icon;
    const void *p = phone ? &icons::phone_icon : &icons::phone_off_icon;
    if (lv_image_get_src(s.wifi) != w) {
        lv_image_set_src(s.wifi, w);
    }
    if (lv_image_get_src(s.phone) != p) {
        lv_image_set_src(s.phone, p);
    }
}

lv_obj_t *button(lv_obj_t *parent, Button kind, const char *glyph, const char *label, bool round,
                 bool checkable)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_add_style(btn, &s_btn, 0);
    lv_obj_add_style(btn, &s_btn_pressed, LV_STATE_PRESSED);
    if (kind == Button::Accent) {
        lv_obj_add_style(btn, &s_btn_accent, 0);
        lv_obj_add_style(btn, &s_btn_accent_pressed, LV_STATE_PRESSED);
    } else if (kind == Button::Flat) {
        lv_obj_add_style(btn, &s_btn_flat, 0);
        lv_obj_add_style(btn, &s_btn_flat_pressed, LV_STATE_PRESSED);
    } else if (kind == Button::Soft) {
        lv_obj_add_style(btn, &s_btn_soft, 0);
    }
    if (round) {
        lv_obj_add_style(btn, &s_btn_round, 0);
    }
    lv_obj_add_style(btn, &s_btn_open, LV_STATE_USER_1);
    if (checkable) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_add_style(btn, &s_btn_checked, LV_STATE_CHECKED);
        lv_obj_add_style(btn, &s_btn_checked_pressed, LV_STATE_CHECKED | LV_STATE_PRESSED);
    }
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    // a transition, even one of no length, only takes its end on the next
    // animation tick, which LVGL runs after it has drawn the frame: run it now,
    // so the press is in the very frame the touch is read for
    lv_obj_add_event_cb(btn, [](lv_event_t *) { lv_anim_refr_now(); }, LV_EVENT_PRESSED, nullptr);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, size::button);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const bool filled = kind == Button::Accent;
    if (glyph != nullptr) {
        lv_obj_t *ic = icon(btn, glyph, 28, filled);
        lv_obj_remove_style(ic, &s_ink[0], 0);  // the button's own ink, so checked and accent recolour it
        lv_obj_add_flag(ic, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
    if (label != nullptr) {
        lv_obj_t *t = lv_label_create(btn);
        lv_obj_add_style(t, &s_role[static_cast<int>(Role::H3)], 0);
        lv_label_set_text(t, label);
    }
    return btn;
}

lv_obj_t *lamp_tile(lv_obj_t *parent, const char *glyph, const char *name, bool on)
{
    lv_obj_t *t = button(parent, Button::Raised, nullptr, nullptr, false, true);
    lv_obj_add_style(t, &s_lamp, 0);
    lv_obj_add_style(t, &s_lamp_on, LV_STATE_CHECKED);
    lv_obj_add_style(t, &s_lamp_on_pressed, LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *ic = icon(t, glyph, 28, on);
    lv_obj_remove_style(ic, &s_ink[0], 0);
    lv_obj_t *col = lv_obj_create(t);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *n = text(col, Role::H3, name);
    lv_obj_remove_style(n, &s_ink[0], 0);
    lv_obj_t *state = text(col, Role::Small, on ? "On" : "Off", Ink::Secondary);
    lv_obj_add_style(state, &s_lamp_on_ink, LV_STATE_CHECKED);
    lv_obj_add_flag(state, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (on) {
        lv_obj_add_state(t, LV_STATE_CHECKED);
        lv_obj_add_state(state, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(
        t,
        [](lv_event_t *e) {
            lv_obj_t *tile = lv_event_get_target_obj(e);
            set_lamp(tile, lv_obj_has_state(tile, LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    return t;
}

void set_lamp(lv_obj_t *t, bool on)
{
    if (on) {
        lv_obj_add_state(t, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(t, LV_STATE_CHECKED);
    }
    lv_obj_t *ic    = lv_obj_get_child(t, 0);
    lv_obj_t *state = lv_obj_get_child(lv_obj_get_child(t, 1), 1);
    set_icon(ic, lv_label_get_text(ic), on);
    if (std::strcmp(lv_label_get_text(state), on ? "On" : "Off") != 0) {
        lv_label_set_text(state, on ? "On" : "Off");
    }
    if (on) {
        lv_obj_add_state(state, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(state, LV_STATE_CHECKED);
    }
}

void set_usable(lv_obj_t *obj, bool usable)
{
    if (obj == nullptr) {
        return;
    }
    const lv_opa_t opa = usable ? LV_OPA_COVER : LV_OPA_40;
    if (lv_obj_is_clickable(obj) == usable && lv_obj_get_style_opa(obj, LV_PART_MAIN) == opa) {
        return;
    }
    lv_obj_set_clickable(obj, usable);
    lv_obj_set_style_opa(obj, opa, 0);
    if (!usable) {
        lv_obj_remove_state(obj, LV_STATE_PRESSED);
    }
}

lv_obj_t *toggle(lv_obj_t *parent, bool on)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_add_style(sw, &s_switch, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, radius::round, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, radius::round, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, radius::round, LV_PART_KNOB);
    lv_obj_add_style(sw, &s_switch_on, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(sw, &s_switch_knob, LV_PART_KNOB);
    // under the finger the knob stretches towards where it would go
    lv_obj_set_style_pad_right(sw, 8, LV_PART_KNOB | LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(sw, 8, LV_PART_KNOB | LV_STATE_PRESSED | LV_STATE_CHECKED);
    lv_obj_set_style_pad_right(sw, -4, LV_PART_KNOB | LV_STATE_PRESSED | LV_STATE_CHECKED);
    lv_obj_set_style_anim_duration(sw, motion::move_ms, 0);
    lv_obj_add_event_cb(
        sw,
        [](lv_event_t *e) {
            // the switch slides its knob in a straight line: on the kit's spring instead
            if (lv_anim_t *a = lv_anim_get(lv_event_get_target_obj(e), nullptr)) {
                lv_anim_set_path_cb(a, motion::spring);
            }
        },
        LV_EVENT_STATE_CHANGED, nullptr);
    lv_obj_set_size(sw, size::toggle_w, size::toggle_h);
    lv_obj_set_ext_click_area(sw, space::s);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

namespace {
struct Seg {
    lv_obj_t *thumb;
    int       chosen;
};

void place_thumb(lv_obj_t *seg, int index, bool animate)
{
    auto     *data  = static_cast<Seg *>(lv_obj_get_user_data(seg));
    lv_obj_t *item  = lv_obj_get_child(seg, index + 1);
    lv_obj_update_layout(seg);
    const std::int32_t x = lv_obj_get_x(item), w = lv_obj_get_width(item);
    data->chosen = index;
    for (std::uint32_t i = 1; i < lv_obj_get_child_count(seg); ++i) {
        lv_obj_t     *it  = lv_obj_get_child(seg, static_cast<int32_t>(i));
        const lv_opa_t opa = static_cast<int>(i) - 1 == index ? static_cast<lv_opa_t>(LV_OPA_COVER) : s_pal.ink2_opa;
        if (lv_obj_get_style_text_opa(it, LV_PART_MAIN) != opa) {
            lv_obj_set_style_text_opa(it, opa, 0);
        }
    }
    lv_anim_delete(data->thumb, nullptr);
    if (!animate) {
        lv_obj_set_x(data->thumb, x);
        lv_obj_set_width(data->thumb, w);
        return;
    }
    for (int k = 0; k < 2; ++k) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, data->thumb);
        lv_anim_set_values(&a, k == 0 ? lv_obj_get_x(data->thumb) : lv_obj_get_width(data->thumb), k == 0 ? x : w);
        lv_anim_set_exec_cb(&a, k == 0 ? reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_x)
                                       : reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_width));
        lv_anim_set_duration(&a, motion::move_ms);
        lv_anim_set_path_cb(&a, motion::spring);
        lv_anim_start(&a);
    }
}

void on_seg_click(lv_event_t *e)
{
    lv_obj_t *item = lv_event_get_current_target_obj(e);
    lv_obj_t *seg  = lv_obj_get_parent(item);
    place_thumb(seg, static_cast<int>(lv_obj_get_index(item)) - 1, true);
    lv_obj_send_event(seg, LV_EVENT_VALUE_CHANGED, nullptr);
}
}  // namespace

lv_obj_t *segmented(lv_obj_t *parent, std::initializer_list<const char *> labels, int chosen, std::int32_t width)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_remove_style_all(seg);
    lv_obj_add_style(seg, &s_seg, 0);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(seg, width, 60);
    lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(seg, 4, 0);
    auto *data   = new Seg{};
    data->thumb  = lv_obj_create(seg);
    lv_obj_remove_style_all(data->thumb);
    lv_obj_add_style(data->thumb, &s_seg_thumb, 0);
    lv_obj_add_flag(data->thumb, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_height(data->thumb, 48);
    lv_obj_remove_flag(data->thumb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(seg, data);
    for (const char *label : labels) {
        lv_obj_t *item = lv_obj_create(seg);
        lv_obj_remove_style_all(item);
        lv_obj_add_style(item, &s_seg_item, 0);
        lv_obj_set_flex_grow(item, 1);
        lv_obj_set_height(item, 48);
        lv_obj_set_ext_click_area(item, 6);  // over the gaps and the well's rim, so no tap on it falls through
        lv_obj_set_style_text_opa(item, 110, LV_STATE_PRESSED);
        lv_obj_t *t = lv_label_create(item);
        lv_obj_add_style(t, &s_role[static_cast<int>(Role::H3)], 0);
        lv_obj_set_style_text_font(t, fonts::small(), 0);
        lv_label_set_text(t, label);
        lv_obj_center(t);
        lv_obj_add_event_cb(item, on_seg_click, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_add_event_cb(
        seg, [](lv_event_t *e) { delete static_cast<Seg *>(lv_obj_get_user_data(lv_event_get_target_obj(e))); },
        LV_EVENT_DELETE, nullptr);
    place_thumb(seg, chosen, false);
    return seg;
}

int segmented_chosen(lv_obj_t *seg) { return static_cast<Seg *>(lv_obj_get_user_data(seg))->chosen; }

void segmented_choose(lv_obj_t *seg, int index, bool animate)
{
    const auto *data = static_cast<Seg *>(lv_obj_get_user_data(seg));
    if (index == data->chosen && lv_anim_get(data->thumb, nullptr) == nullptr) {
        return;  // there already
    }
    place_thumb(seg, index, animate);
}

lv_obj_t *capsule(lv_obj_t *parent)
{
    lv_obj_t *cap = glass(parent, Glass::Small);
    lv_obj_set_style_radius(cap, radius::round, 0);
    lv_obj_set_size(cap, LV_SIZE_CONTENT, size::bar);
    lv_obj_set_style_pad_hor(cap, space::s, 0);
    lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return cap;
}

lv_obj_t *capsule_item(lv_obj_t *cap, const char *glyph, const char *label, std::int32_t width)
{
    lv_obj_t *item = button(cap, Button::Flat, glyph, nullptr, true);
    lv_obj_set_size(item, width, size::button);
    lv_obj_set_style_pad_column(item, space::s, 0);
    if (label != nullptr) {
        lv_obj_t *t = lv_label_create(item);
        lv_obj_add_style(t, &s_role[static_cast<int>(Role::H2)], 0);
        lv_label_set_text(t, label);
    }
    return item;
}

void capsule_divider(lv_obj_t *cap) { hairline(cap, true, 30); }

}  // namespace ui::kit
