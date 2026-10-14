#include "kit_bake.h"

#include "kit.h"

#include "esp_heap_caps.h"

#include "kit_pixels.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ui::kit::bake {
namespace {

constexpr std::uint32_t REST_MS = 200;  // under the popovers' 250 ms, so their picture is taken after
constexpr std::int32_t  STRIP   = 32;   // rows of shadow drawn at a time, to keep the scratch small

struct Pose {
    lv_area_t     coords{}, clip{};
    lv_opa_t      opa   = 0;
    bool          shown = false;
    std::uint32_t look  = 0;
};

bool meet(lv_area_t *out, const lv_area_t &a, const lv_area_t &b)
{
    *out = {std::max(a.x1, b.x1), std::max(a.y1, b.y1), std::min(a.x2, b.x2), std::min(a.y2, b.y2)};
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

bool overlap(const lv_area_t &a, const lv_area_t &b)
{
    lv_area_t unused;
    return meet(&unused, a, b);
}

bool same(const lv_area_t &a, const lv_area_t &b)
{
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

bool same(const Pose &a, const Pose &b)
{
    return same(a.coords, b.coords) && same(a.clip, b.clip) && a.opa == b.opa && a.shown == b.shown &&
           a.look == b.look;
}

// A shadow opacity an object had of its own, to give back once its shadow is live again.
struct OwnShadow {
    bool     had = false;
    lv_opa_t opa = 0;
};

struct Caster {
    lv_obj_t     *tile = nullptr;
    Pose          seen, baked_at;
    std::uint32_t still_since = 0;
    lv_area_t     painted{};  // where its shadow is in the picture
    lv_area_t     body{};     // the tile it fell from, as it stood
    std::int32_t  corner  = 0;
    bool          cover   = false;  // opaque, so nothing was painted along its sides under it
    std::int32_t  radius  = 0;
    bool          baked   = false;
    bool          refused = false;  // as it stands now; asked again once it moves
    OwnShadow     own;
    bool          filled = false;  // its content covers it to its rounded edge, though its own ground is clear
};

KIT_IN_PSRAM Caster s_casters[96];
int            s_count   = 0;
lv_obj_t      *s_ground  = nullptr, *s_content = nullptr, *s_canvas = nullptr;
lv_draw_buf_t *s_picture = nullptr, *s_strip = nullptr;  // in PSRAM on the panel
void (*s_repaint)(const lv_area_t &area) = nullptr;
std::uint32_t  s_look    = 1;

Pose pose_of(lv_obj_t *tile)
{
    Pose p;
    p.look = s_look;
    lv_obj_get_coords(tile, &p.coords);
    lv_display_t *disp = lv_obj_get_display(tile);
    p.clip  = {0, 0, lv_display_get_horizontal_resolution(disp) - 1, lv_display_get_vertical_resolution(disp) - 1};
    p.shown = lv_obj_get_screen(tile) == lv_display_get_screen_active(disp);
    for (lv_obj_t *o = tile; o != nullptr && p.shown; o = lv_obj_get_parent(o)) {
        if (lv_obj_is_hidden(o)) {
            p.shown = false;
        } else if (o != tile && !lv_obj_is_overflow_visible(o)) {
            lv_area_t a;
            lv_obj_get_coords(o, &a);
            p.shown = meet(&p.clip, p.clip, a);
        }
    }
    p.opa = lv_obj_get_style_opa_recursive(tile, LV_PART_MAIN);
    return p;
}

bool transparent(lv_obj_t *o)
{
    return lv_obj_get_style_bg_opa(o, LV_PART_MAIN) <= LV_OPA_MIN &&
           (lv_obj_get_style_bg_image_src(o, LV_PART_MAIN) == nullptr ||
            lv_obj_get_style_bg_image_opa(o, LV_PART_MAIN) <= LV_OPA_MIN) &&
           (lv_obj_get_style_border_width(o, LV_PART_MAIN) == 0 || lv_obj_get_style_border_opa(o, LV_PART_MAIN) <= LV_OPA_MIN) &&
           (lv_obj_get_style_outline_width(o, LV_PART_MAIN) == 0 || lv_obj_get_style_outline_opa(o, LV_PART_MAIN) <= LV_OPA_MIN) &&
           (lv_obj_get_style_shadow_width(o, LV_PART_MAIN) == 0 || lv_obj_get_style_shadow_opa(o, LV_PART_MAIN) <= LV_OPA_MIN);
}

bool flat(lv_obj_t *o)
{
    return lv_obj_get_style_transform_scale_x(o, LV_PART_MAIN) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_scale_y(o, LV_PART_MAIN) == LV_SCALE_NONE &&
           lv_obj_get_style_transform_rotation(o, LV_PART_MAIN) == 0 &&
           lv_obj_get_style_opa_layered(o, LV_PART_MAIN) >= LV_OPA_MAX;
}

// What the shadow falls on must be the ground and nothing else: every holder
// of the tile, up to the content plane, draws nothing of its own.
bool on_the_ground(lv_obj_t *tile)
{
    if (!flat(tile)) {
        return false;
    }
    for (lv_obj_t *o = lv_obj_get_parent(tile); o != nullptr; o = lv_obj_get_parent(o)) {
        if (o == s_content) {
            return true;
        }
        if (!lv_obj_check_type(o, &lv_obj_class) || !transparent(o) || !flat(o)) {
            return false;
        }
    }
    return false;
}

Caster *caster_of(lv_obj_t *o)
{
    for (int i = 0; i < s_count; ++i) {
        if (s_casters[i].tile == o) {
            return &s_casters[i];
        }
    }
    return nullptr;
}

bool is_holder(lv_obj_t *o, lv_obj_t *tile)
{
    for (lv_obj_t *p = lv_obj_get_parent(tile); p != nullptr; p = lv_obj_get_parent(p)) {
        if (p == o) {
            return true;
        }
    }
    return false;
}

// Anything drawn before the tile that reaches into its shadow would, painted
// in, end up over the shadow rather than under it. Another tile's shadow is
// the same colour as this one's, so those two can lie in either order.
bool blocks(lv_obj_t *o, const lv_area_t &box)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const lv_area_t near{a.x1 - 100, a.y1 - 100, a.x2 + 100, a.y2 + 100};  // further than any kit shadow reaches
    if (!overlap(near, box)) {
        return false;
    }
    if (caster_of(o) == nullptr) {
        if (lv_obj_check_type(o, &lv_obj_class) && transparent(o)) {
            return false;
        }
        const std::int32_t ext = lv_obj_calculate_ext_draw_size(o, LV_PART_MAIN) +
                                 lv_obj_get_style_line_width(o, LV_PART_MAIN);
        lv_area_increase(&a, ext, ext);
    }
    return overlap(a, box);
}

enum class Walk : std::uint8_t { On, Found, Blocked };

Walk walk(lv_obj_t *o, lv_obj_t *tile, const lv_area_t &box)
{
    const std::uint32_t n = lv_obj_get_child_count(o);
    for (std::uint32_t i = 0; i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(o, static_cast<std::int32_t>(i));
        if (c == tile) {
            return Walk::Found;
        }
        if (lv_obj_is_hidden(c)) {
            continue;
        }
        const bool holder = is_holder(c, tile);
        if (!holder && blocks(c, box)) {
            return Walk::Blocked;
        }
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        if (!holder && !lv_obj_is_overflow_visible(c) && !overlap(a, box)) {
            continue;  // what it holds is cut to it, so cannot reach the shadow either
        }
        const Walk w = walk(c, tile, box);
        if (w != Walk::On) {
            return w;
        }
    }
    return Walk::On;
}

// The style change alone would redraw the whole object, where only its rim
// changes, or nothing at all.
// The shadow is put out as the tile's own property, not as a style added: a
// style refreshes every property of the tile's children too, 18 ms for the
// week's, where a shadow's opacity is not inherited and touches the tile alone.
// One the tile had of its own is kept to give back.
void quietly(lv_obj_t *obj, bool settle, OwnShadow &own)
{
    lv_display_t *disp = lv_obj_get_display(obj);
    lv_display_enable_invalidation(disp, false);
    if (settle) {
        lv_style_value_t value{};
        own.had = lv_obj_get_local_style_prop(obj, LV_STYLE_SHADOW_OPA, &value, 0) == LV_RESULT_OK;
        own.opa = static_cast<lv_opa_t>(value.num);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, 0);
    } else if (own.had) {
        lv_obj_set_style_shadow_opa(obj, own.opa, 0);
    } else {
        lv_obj_remove_local_style_prop(obj, LV_STYLE_SHADOW_OPA, 0);
    }
    lv_display_enable_invalidation(disp, true);
}

bool scratch()
{
    if (s_strip == nullptr) {
        s_strip = lv_draw_buf_create(static_cast<std::uint32_t>(s_picture->header.w), STRIP, LV_COLOR_FORMAT_ARGB8888,
                                     LV_STRIDE_AUTO);
        if (s_strip == nullptr) {
            return false;
        }
    }
    lv_canvas_set_draw_buf(s_canvas, s_strip);
    return true;
}

// One strip of a shadow, `y0` rows down `box`, drawn by LVGL as the object
// would draw it, into the transparent scratch.
void shadow_strip(const lv_draw_box_shadow_dsc_t &dsc, const lv_area_t &body, const lv_area_t &box, std::int32_t y0,
                  std::int32_t x1, std::int32_t x2)
{
    lv_draw_buf_clear(s_strip, nullptr);
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    layer._clip_area = {x1 - box.x1, 0, x2 - box.x1, STRIP - 1};
    layer.phy_clip_area = layer._clip_area;
    lv_area_t at = body;
    lv_area_move(&at, -box.x1, -y0);
    lv_draw_box_shadow(&layer, &dsc, &at);
    lv_canvas_finish_layer(s_canvas, &layer);
}

// The shadow shows only outside the tile's rounded body, so that is all that needs drawing again.
void invalidate_rim(const lv_area_t &box, const lv_area_t &body, std::int32_t r)
{
    const lv_area_t inner{body.x1 + r, body.y1 + r, body.x2 - r, body.y2 - r};
    if (inner.x2 < inner.x1 || inner.y2 < inner.y1) {
        lv_obj_invalidate_area(s_ground, &box);
        return;
    }
    const lv_area_t parts[] = {{box.x1, box.y1, box.x2, inner.y1 - 1},
                               {box.x1, inner.y2 + 1, box.x2, box.y2},
                               {box.x1, inner.y1, inner.x1 - 1, inner.y2},
                               {inner.x2 + 1, inner.y1, box.x2, inner.y2}};
    for (const lv_area_t &p : parts) {
        lv_area_t a;
        if (meet(&a, p, box)) {
            lv_obj_invalidate_area(s_ground, &a);
        }
    }
}

// The parts of `box` a shadow is painted into, a strip at a time. Beside an
// opaque tile, in any of its rows, only the columns either side: between its
// corners' columns the tile covers every row it has, its rounded corners too.
template <typename Fn>
void each_painted(const lv_area_t &box, const lv_area_t &body, std::int32_t corner, bool cover, Fn &&fn)
{
    const bool narrow = body.x2 - body.x1 <= 2 * corner;
    for (std::int32_t y0 = box.y1; y0 <= box.y2; y0 += STRIP) {
        const std::int32_t y1 = std::min(y0 + STRIP, box.y2 + 1);
        if (!cover || narrow) {
            fn(y0, y1 - y0, box.x1, box.x2);
            continue;
        }
        // above the tile, beside it, below it
        const std::int32_t in1 = std::clamp(body.y1, y0, y1), in2 = std::clamp(body.y2 + 1, y0, y1);
        if (in1 > y0) {
            fn(y0, in1 - y0, box.x1, box.x2);
        }
        if (in2 > in1) {
            fn(in1, in2 - in1, box.x1, body.x1 + corner);
            fn(in1, in2 - in1, body.x2 - corner, box.x2);
        }
        if (y1 > in2) {
            fn(in2, y1 - in2, box.x1, box.x2);
        }
    }
}

std::int32_t corner_of(std::int32_t radius, const lv_area_t &body)
{
    return std::min({radius, lv_area_get_width(&body) / 2, lv_area_get_height(&body) / 2}) + 1;
}

void restore(const Caster &c)
{
    each_painted(c.painted, c.body, c.corner, c.cover, [](std::int32_t y0, std::int32_t rows, std::int32_t x1, std::int32_t x2) {
        s_repaint({x1, y0, x2, y0 + rows - 1});
    });
    lv_draw_buf_flush_cache(s_picture, &c.painted);
}

void unbake(Caster &c)
{
    if (!c.baked) {
        return;
    }
    c.baked = false;
    restore(c);
    invalidate_rim(c.painted, c.baked_at.coords, c.radius);
    quietly(c.tile, false, c.own);
    for (int i = 0; i < s_count; ++i) {
        if (s_casters[i].baked && overlap(s_casters[i].painted, c.painted)) {
            unbake(s_casters[i]);  // the restoring wiped its shadow where the two meet
        }
    }
}

// A shadow's blur is most of what a page change costs, and a page's tiles
// come back in the same shapes: each shape's shadow is kept as the alpha of
// the pixels painted, in the order they are painted, and laid down again.
struct Shade {
    std::int32_t  w = 0, h = 0;                    // the tile's
    lv_area_t     box{};                           // the painted box, from the tile's corner
    std::int32_t  radius = 0, width = 0, spread = 0, ofs_x = 0, ofs_y = 0;
    lv_opa_t      opa   = 0;
    bool          cover = false;
    std::uint8_t *alpha = nullptr;
    std::size_t   bytes = 0;
};
constexpr int         SHADES       = 24;
constexpr std::size_t SHADES_BYTES = 1536 * 1024;
KIT_IN_PSRAM Shade    s_shades[SHADES];
int                   s_next_shade = 0;
std::size_t           s_shade_bytes = 0;

Shade shape_of(const lv_draw_box_shadow_dsc_t &dsc, const lv_area_t &body, const lv_area_t &box)
{
    Shade k;
    k.w      = lv_area_get_width(&body);
    k.h      = lv_area_get_height(&body);
    k.box    = {box.x1 - body.x1, box.y1 - body.y1, box.x2 - body.x1, box.y2 - body.y1};
    k.radius = dsc.radius;
    k.width  = dsc.width;
    k.spread = dsc.spread;
    k.ofs_x  = dsc.ofs_x;
    k.ofs_y  = dsc.ofs_y;
    k.opa    = dsc.opa;
    k.cover  = dsc.bg_cover != 0;
    return k;
}

bool same(const Shade &a, const Shade &b)
{
    return a.w == b.w && a.h == b.h && same(a.box, b.box) && a.radius == b.radius && a.width == b.width &&
           a.spread == b.spread && a.ofs_x == b.ofs_x && a.ofs_y == b.ofs_y && a.opa == b.opa && a.cover == b.cover;
}

std::size_t painted_bytes(const lv_area_t &box, const lv_area_t &body, std::int32_t corner, bool cover)
{
    std::size_t n = 0;
    each_painted(box, body, corner, cover, [&](std::int32_t, std::int32_t rows, std::int32_t x1, std::int32_t x2) {
        n += static_cast<std::size_t>(rows) * static_cast<std::size_t>(x2 - x1 + 1);
    });
    return n;
}

// A slot for a new shape, its alpha `bytes` long; null when there is no room.
Shade *keep_shade(const Shade &shape, std::size_t bytes)
{
    Shade &slot = s_shades[s_next_shade];
    if (slot.alpha != nullptr) {
        heap_caps_free(slot.alpha);
        s_shade_bytes -= slot.bytes;
        slot.alpha = nullptr;
    }
    if (s_shade_bytes + bytes > SHADES_BYTES) {
        return nullptr;
    }
    auto *alpha = static_cast<std::uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (alpha == nullptr) {
        return nullptr;
    }
    slot          = shape;
    slot.alpha    = alpha;
    slot.bytes    = bytes;
    s_shade_bytes += bytes;
    s_next_shade  = (s_next_shade + 1) % SHADES;
    return &slot;
}

// The shadow as LVGL draws it, laid on the ground in full colour and
// dithered: the same shadow, without the bands RGB565 cuts a soft one into.
void paint(const lv_draw_box_shadow_dsc_t &dsc, const lv_area_t &body, const lv_area_t &box)
{
    const std::uint32_t stride = s_picture->header.stride;
    const int sr = static_cast<int>((dsc.color.red)), sg = static_cast<int>(dsc.color.green),
              sb = static_cast<int>(dsc.color.blue);
    const std::int32_t corner = corner_of(dsc.radius, body);
    const bool         cover  = dsc.bg_cover != 0;
    const Shade        shape  = shape_of(dsc, body, box);
    const Shade       *kept   = nullptr;
    for (const Shade &k : s_shades) {
        if (k.alpha != nullptr && same(k, shape)) {
            kept = &k;
            break;
        }
    }
    Shade *making = kept == nullptr ? keep_shade(shape, painted_bytes(box, body, corner, cover)) : nullptr;
    std::size_t at = 0;
    each_painted(box, body, corner, cover,
                 [&](std::int32_t y0, std::int32_t rows, std::int32_t x1, std::int32_t x2) {
        if (kept == nullptr) {
            shadow_strip(dsc, body, box, y0, x1, x2);
        }
        for (std::int32_t r = 0; r < rows; ++r) {
            const std::uint8_t *in  = s_strip->data + static_cast<std::size_t>(r) * s_strip->header.stride;
            auto              *out = reinterpret_cast<std::uint16_t *>(s_picture->data + static_cast<std::size_t>(y0 + r) * stride) + box.x1;
            for (std::int32_t x = x1 - box.x1; x <= x2 - box.x1; ++x) {
                int a = 0;
                if (kept != nullptr) {
                    a = kept->alpha[at++];
                } else {
                    a = in[x * 4 + 3];
                    if (making != nullptr) {
                        making->alpha[at++] = static_cast<std::uint8_t>(a);
                    }
                }
                if (a == 0) {
                    continue;
                }
                // at the bottom of each step, so a step dithers back to itself
                const std::uint16_t g = out[x];
                const int gr = (g >> 11) << 3, gg = ((g >> 5) & 63) << 2, gb = (g & 31) << 3;
                out[x] = pixels::dither((sr * a + gr * (255 - a) + 127) / 255, (sg * a + gg * (255 - a) + 127) / 255,
                                        (sb * a + gb * (255 - a) + 127) / 255, box.x1 + x, y0 + r);
            }
        }
    });
    lv_draw_buf_flush_cache(s_picture, &box);
}

bool bake(Caster &c, const Pose &pose)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    lv_obj_init_draw_rect_dsc(c.tile, LV_PART_MAIN, &d);
    if (d.shadow_width == 0 || d.shadow_opa <= LV_OPA_MIN || !on_the_ground(c.tile)) {
        return false;
    }
    lv_draw_box_shadow_dsc_t s;
    lv_draw_box_shadow_dsc_init(&s);
    s.radius = d.radius;
    s.color  = d.shadow_color;
    s.width  = d.shadow_width;
    s.spread = d.shadow_spread;
    s.ofs_x  = d.shadow_offset_x;
    s.ofs_y  = d.shadow_offset_y;
    s.opa    = d.shadow_opa;
    // as lv_draw_rect decides it: under an opaque tile the shadow goes unmasked
    bool cover = d.bg_opa >= LV_OPA_COVER;
    if (d.bg_grad.dir != LV_GRAD_DIR_NONE) {
        for (std::uint32_t i = 0; i < d.bg_grad.stops_count; ++i) {
            cover = cover && d.bg_grad.stops[i].opa >= LV_OPA_COVER;
        }
    }
    s.bg_cover = cover || c.filled ? 1 : 0;

    const std::int32_t reach = s.width / 2 + 1;
    lv_area_t box{pose.coords.x1 + s.ofs_x - s.spread - reach, pose.coords.y1 + s.ofs_y - s.spread - reach,
                  pose.coords.x2 + s.ofs_x + s.spread + reach, pose.coords.y2 + s.ofs_y + s.spread + reach};
    if (!meet(&box, box, pose.clip) || walk(s_content, c.tile, box) != Walk::Found) {
        return false;
    }
    if (!scratch()) {
        return false;
    }
    paint(s, pose.coords, box);
    c.body     = pose.coords;
    c.corner   = corner_of(s.radius, pose.coords);
    c.cover    = s.bg_cover != 0;
    c.radius   = std::min({s.radius, lv_area_get_width(&pose.coords) / 2, lv_area_get_height(&pose.coords) / 2}) + 1;
    c.baked    = true;
    c.painted  = box;
    c.baked_at = pose;
    quietly(c.tile, true, c.own);
    invalidate_rim(box, pose.coords, c.radius);
    return true;
}

// --- glass ---------------------------------------------------------------

struct Shape {
    std::int32_t  w = 0, h = 0, radius = 0, width = 0, spread = 0, ofs_x = 0, ofs_y = 0;
    std::int32_t  rim = 0;  // how far the outline reaches out
    lv_opa_t      opa = 0;
    lv_color_t    colour{};
    std::uint32_t look = 0;
};

bool same(const Shape &a, const Shape &b)
{
    return a.w == b.w && a.h == b.h && a.radius == b.radius && a.width == b.width && a.spread == b.spread &&
           a.ofs_x == b.ofs_x && a.ofs_y == b.ofs_y && a.opa == b.opa && lv_color_eq(a.colour, b.colour) &&
           a.rim == b.rim && a.look == b.look;
}

struct Pane {
    lv_obj_t         *obj  = nullptr;
    const lv_style_t *base = nullptr;
    Shape             seen, drawn;
    std::uint32_t     still_since = 0;
    lv_draw_buf_t    *picture = nullptr;  // A8: how much of the shadow falls on each pixel round the pane
    lv_area_t         box{};              // where the picture lies, from the pane's corner
    bool              carried = false, refused = false;
    bool              trimmable = false;  // drawn untransformed and unblurred, so LVGL's box round it is plain
    OwnShadow         own;
};

KIT_IN_PSRAM Pane s_panes[24];
int  s_pane_count = 0;

Pane *pane_of(lv_obj_t *o)
{
    for (int i = 0; i < s_pane_count; ++i) {
        if (s_panes[i].obj == o) {
            return &s_panes[i];
        }
    }
    return nullptr;
}

// What the pane's own style asks for, rather than what is left once the
// carried picture has taken the shadow over.
lv_opa_t shadow_opa(const Pane &p)
{
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(p.obj, LV_STYLE_SHADOW_OPA, &v, 0) == LV_STYLE_RES_FOUND ||
        lv_style_get_prop(p.base, LV_STYLE_SHADOW_OPA, &v) == LV_STYLE_RES_FOUND) {
        return static_cast<lv_opa_t>(v.num);
    }
    return LV_OPA_COVER;
}

Shape shape_of(const Pane &p)
{
    Shape s;
    s.w      = lv_obj_get_width(p.obj);
    s.h      = lv_obj_get_height(p.obj);
    s.radius = lv_obj_get_style_radius(p.obj, LV_PART_MAIN);
    s.width  = lv_obj_get_style_shadow_width(p.obj, LV_PART_MAIN);
    s.spread = lv_obj_get_style_shadow_spread(p.obj, LV_PART_MAIN);
    s.ofs_x  = lv_obj_get_style_shadow_offset_x(p.obj, LV_PART_MAIN);
    s.ofs_y  = lv_obj_get_style_shadow_offset_y(p.obj, LV_PART_MAIN);
    s.opa    = shadow_opa(p);
    s.colour = lv_obj_get_style_shadow_color(p.obj, LV_PART_MAIN);
    s.rim    = lv_obj_get_style_outline_opa(p.obj, LV_PART_MAIN) > LV_OPA_MIN
                   ? lv_obj_get_style_outline_width(p.obj, LV_PART_MAIN) + lv_obj_get_style_outline_pad(p.obj, LV_PART_MAIN)
                   : 0;
    s.look   = s_look;
    return s;
}

std::int32_t reach_of(const Shape &s)
{
    return s.width / 2 + 1 + s.spread + std::max(std::abs(s.ofs_x), std::abs(s.ofs_y));
}

// The shadow of a pane this shape, once, so the pane only lays it down: a
// live one works its soft corners out again on every frame anything near it
// changes, and masks the glass out of itself row by row.
bool draw_picture(Pane &p, const Shape &s)
{
    if (s.width == 0 || s.opa <= LV_OPA_MIN || s.w <= 0 || s.h <= 0 ||
        lv_obj_get_style_blur_backdrop(p.obj, LV_PART_MAIN)) {
        return false;
    }
    const std::int32_t reach = s.width / 2 + 1;
    const lv_area_t    box{s.ofs_x - s.spread - reach, s.ofs_y - s.spread - reach, s.w - 1 + s.ofs_x + s.spread + reach,
                        s.h - 1 + s.ofs_y + s.spread + reach};
    const auto w = static_cast<std::uint32_t>(lv_area_get_width(&box));
    const auto h = static_cast<std::uint32_t>(lv_area_get_height(&box));
    if (!scratch() || w > s_strip->header.w) {
        return false;
    }
    if (p.picture == nullptr || p.picture->header.w != w || p.picture->header.h != h) {
        if (p.picture != nullptr) {
            lv_draw_buf_destroy(p.picture);
        }
        p.picture = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
        if (p.picture == nullptr) {
            return false;
        }
    }
    lv_draw_box_shadow_dsc_t dsc;
    lv_draw_box_shadow_dsc_init(&dsc);
    dsc.radius   = s.radius;
    dsc.color    = lv_color_black();
    dsc.width    = s.width;
    dsc.spread   = s.spread;
    dsc.ofs_x    = s.ofs_x;
    dsc.ofs_y    = s.ofs_y;
    dsc.opa      = s.opa;
    dsc.bg_cover = 0;  // glass lets what is under it show, so its shadow stops at its edge
    const lv_area_t body{0, 0, s.w - 1, s.h - 1};
    for (std::int32_t y0 = box.y1; y0 <= box.y2; y0 += STRIP) {
        shadow_strip(dsc, body, box, y0, box.x1, box.x2);
        const std::int32_t rows = std::min(STRIP, box.y2 - y0 + 1);
        for (std::int32_t r = 0; r < rows; ++r) {
            const std::uint8_t *in  = s_strip->data + static_cast<std::size_t>(r) * s_strip->header.stride;
            std::uint8_t       *out = p.picture->data + static_cast<std::size_t>(y0 - box.y1 + r) * p.picture->header.stride;
            for (std::uint32_t x = 0; x < w; ++x) {
                out[x] = in[x * 4 + 3];
            }
        }
    }
    p.box = box;
    return true;
}

void on_pane_ext(lv_event_t *e)
{
    const Pane *p = pane_of(lv_event_get_target_obj(e));
    if (p != nullptr && p->carried) {
        lv_event_set_ext_draw_size(e, reach_of(p->drawn));
    }
}

void on_pane_draw(lv_event_t *e)
{
    const Pane *p = pane_of(lv_event_get_target_obj(e));
    if (p == nullptr || !p->carried) {
        return;
    }
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   c;
    lv_obj_get_coords(p->obj, &c);
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src     = p->picture;
    d.recolor = p->drawn.colour;
    d.opa     = lv_obj_get_style_opa_recursive(p->obj, LV_PART_MAIN);
    const lv_area_t at{c.x1 + p->box.x1, c.y1 + p->box.y1, c.x1 + p->box.x2, c.y1 + p->box.y2};
    // only round the pane: under it the picture is empty
    const std::int32_t r = std::min({p->drawn.radius, p->drawn.w / 2, p->drawn.h / 2}) + 1;
    const lv_area_t    inner{c.x1 + r, c.y1 + r, c.x2 - r, c.y2 - r};
    const lv_area_t    bands[] = {{at.x1, at.y1, at.x2, inner.y1 - 1},
                                  {at.x1, inner.y2 + 1, at.x2, at.y2},
                                  {at.x1, inner.y1, inner.x1 - 1, inner.y2},
                                  {inner.x2 + 1, inner.y1, at.x2, inner.y2}};
    const lv_area_t    clip = layer->_clip_area;
    for (const lv_area_t &band : bands) {
        if (meet(&layer->_clip_area, clip, band)) {
            lv_draw_image(layer, &d, &at);
        }
    }
    layer->_clip_area = clip;
}

void on_pane_delete(lv_event_t *e)
{
    Pane *p = pane_of(lv_event_get_target_obj(e));
    if (p == nullptr) {
        return;
    }
    if (p->picture != nullptr) {
        lv_draw_buf_destroy(p->picture);
    }
    *p = s_panes[--s_pane_count];
    s_panes[s_pane_count] = Pane{};
}

// LVGL redraws round a shadowed object as far as the shadow reaches at its
// furthest, on every side alike: a pane's shadow falls well below it but
// hardly above or beside it. When the area LVGL asks for is exactly that box
// round a pane, as it is when the pane moves, ask only for what the pane and
// its shadow cover.
void on_invalidate(lv_event_t *e)
{
    auto *area = static_cast<lv_area_t *>(lv_event_get_param(e));
    for (int i = 0; i < s_pane_count; ++i) {
        const Pane  &p = s_panes[i];
        const Shape &s = p.seen;
        if (!p.trimmable || s.width == 0 || s.opa <= LV_OPA_MIN) {
            continue;
        }
        lv_area_t c;
        lv_obj_get_coords(p.obj, &c);
        if (lv_area_get_width(&c) != s.w || lv_area_get_height(&c) != s.h) {
            continue;
        }
        const std::int32_t ext = std::max(reach_of(s), s.rim);
        const lv_area_t    all{c.x1 - ext, c.y1 - ext, c.x2 + ext, c.y2 + ext};
        if (area->x1 < all.x1 || area->y1 < all.y1 || area->x2 > all.x2 || area->y2 > all.y2) {
            continue;
        }
        // as LVGL cut it down: to the pane's holders and to the screen
        const lv_area_t clip = pose_of(p.obj).clip;
        lv_area_t       asked;
        if (!meet(&asked, all, clip) || !same(asked, *area)) {
            continue;
        }
        const std::int32_t reach = s.width / 2 + 1;
        lv_area_t needed{std::min(c.x1 + s.ofs_x - s.spread - reach, c.x1 - s.rim),
                         std::min(c.y1 + s.ofs_y - s.spread - reach, c.y1 - s.rim),
                         std::max(c.x2 + s.ofs_x + s.spread + reach, c.x2 + s.rim),
                         std::max(c.y2 + s.ofs_y + s.spread + reach, c.y2 + s.rim)};
        if (meet(&needed, needed, clip)) {
            *area = needed;
        }
        return;
    }
}

void follow(std::uint32_t now)
{
    for (int i = 0; i < s_count; ++i) {
        Caster    &c    = s_casters[i];
        const Pose pose = pose_of(c.tile);
        if (c.baked && !same(pose, c.baked_at)) {
            unbake(c);
            lv_obj_invalidate(c.tile);  // its live shadow where it now is
        }
        if (!same(pose, c.seen)) {
            c.seen        = pose;
            c.still_since = now;
            c.refused     = false;
        }
    }
}

void on_refresh(lv_event_t *)
{
    if (s_count == 0 && s_pane_count == 0) {
        return;
    }
    lv_obj_update_layout(lv_screen_active());
    const std::uint32_t now = lv_tick_get();
    follow(now);
    for (int i = 0; i < s_pane_count; ++i) {
        Pane       &p     = s_panes[i];
        const Shape shape = shape_of(p);
        if (p.carried && !same(shape, p.drawn)) {
            p.carried = false;  // live again while it changes shape
            quietly(p.obj, false, p.own);
            lv_obj_invalidate(p.obj);
        }
        if (!same(shape, p.seen)) {
            p.seen        = shape;
            p.still_since = now;
            p.refused     = false;
        }
        p.trimmable = !lv_obj_get_style_blur_backdrop(p.obj, LV_PART_MAIN) &&
                      lv_obj_get_style_transform_scale_x(p.obj, LV_PART_MAIN) == LV_SCALE_NONE &&
                      lv_obj_get_style_transform_scale_y(p.obj, LV_PART_MAIN) == LV_SCALE_NONE &&
                      lv_obj_get_style_transform_rotation(p.obj, LV_PART_MAIN) == 0;
    }
}

void carry_panes()
{
    for (int i = 0; i < s_pane_count; ++i) {
        Pane &p = s_panes[i];
        if (p.carried || p.refused || lv_tick_elaps(p.still_since) < REST_MS) {
            continue;
        }
        const Shape shape = shape_of(p);
        if (!same(shape, p.seen)) {
            p.seen        = shape;
            p.still_since = lv_tick_get();
            continue;
        }
        if (!draw_picture(p, shape)) {
            p.refused = true;
            continue;
        }
        // the picture is the shadow as it was drawn, so nothing needs drawing again
        p.drawn   = shape;
        p.carried = true;
        quietly(p.obj, true, p.own);
    }
}

void on_tick(lv_timer_t *)
{
    if (s_picture != nullptr) {
        carry_panes();
    }
    bool due = false;
    for (int i = 0; i < s_count && !due; ++i) {
        const Caster &c = s_casters[i];
        due = !c.baked && !c.refused && c.seen.shown && lv_tick_elaps(c.still_since) >= REST_MS;
    }
    if (!due || s_repaint == nullptr) {
        return;
    }
    lv_obj_update_layout(lv_screen_active());
    for (int i = 0; i < s_count; ++i) {
        Caster &c = s_casters[i];
        if (c.baked || c.refused || !c.seen.shown || lv_tick_elaps(c.still_since) < REST_MS) {
            continue;
        }
        const Pose pose = pose_of(c.tile);
        if (!same(pose, c.seen)) {
            c.seen        = pose;
            c.still_since = lv_tick_get();
            continue;
        }
        c.refused = !bake(c, pose);
    }
}

void on_delete(lv_event_t *e)
{
    Caster *c = caster_of(lv_event_get_target_obj(e));
    if (c == nullptr) {
        return;
    }
    unbake(*c);
    *c = s_casters[--s_count];
}

}  // namespace

void begin(lv_obj_t *ground, lv_obj_t *content, lv_draw_buf_t *picture, void (*repaint)(const lv_area_t &area))
{
    if (s_ground == nullptr) {
        lv_display_add_event_cb(lv_obj_get_display(ground), on_refresh, LV_EVENT_REFR_START, nullptr);
        lv_display_add_event_cb(lv_obj_get_display(ground), on_invalidate, LV_EVENT_INVALIDATE_AREA, nullptr);
        lv_timer_create(on_tick, 100, nullptr);
    }
    s_ground  = ground;
    s_content = content;
    s_picture = picture;
    s_repaint = repaint;
    s_canvas  = lv_canvas_create(ground);
    lv_obj_set_hidden(s_canvas, true);
    painted();
}

void painted()
{
    if (s_picture == nullptr) {
        return;
    }
    for (int i = 0; i < s_count; ++i) {
        if (s_casters[i].baked) {
            s_casters[i].baked = false;
            quietly(s_casters[i].tile, false, s_casters[i].own);
        }
    }
}

void glass(lv_obj_t *obj, const lv_style_t *base)
{
    if (s_ground == nullptr || s_pane_count == static_cast<int>(std::size(s_panes))) {
        return;
    }
    Pane &p       = s_panes[s_pane_count++];
    p             = Pane{};
    p.obj         = obj;
    p.base        = base;
    p.still_since = lv_tick_get();
    lv_obj_add_event_cb(obj, on_pane_ext, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    lv_obj_add_event_cb(obj, on_pane_draw, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
    lv_obj_add_event_cb(obj, on_pane_delete, LV_EVENT_DELETE, nullptr);
}

void restyle()
{
    ++s_look;
    for (int i = 0; i < s_count; ++i) {
        unbake(s_casters[i]);
    }
}

void now()
{
    if (s_repaint == nullptr) {
        return;
    }
    lv_obj_update_layout(lv_screen_active());
    follow(lv_tick_get());
    for (int i = 0; i < s_count; ++i) {
        Caster &c = s_casters[i];
        if (!c.baked && !c.refused && c.seen.shown) {
            c.refused = !bake(c, c.seen);
        }
    }
}

void covered(lv_obj_t *tile)
{
    for (int i = 0; i < s_count; ++i) {
        if (s_casters[i].tile == tile) {
            s_casters[i].filled = true;
        }
    }
}

void cast(lv_obj_t *tile)
{
    if (s_ground == nullptr || s_count == static_cast<int>(std::size(s_casters))) {
        return;
    }
    Caster &c     = s_casters[s_count++];
    c             = Caster{};
    c.tile        = tile;
    c.still_since = lv_tick_get();
    lv_obj_add_event_cb(tile, on_delete, LV_EVENT_DELETE, nullptr);
}

}  // namespace ui::kit::bake
