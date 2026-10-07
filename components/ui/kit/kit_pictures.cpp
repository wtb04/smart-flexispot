#include "kit.h"

#include "kit_fonts.h"
#include "kit_pixels.h"
#include "lvgl_private.h"  // a layer's clip area, to lay down parts of one picture, and the cover check's

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(ESP_PLATFORM)
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#else
#include <chrono>
#include <thread>
#endif

namespace ui::kit {

struct Backdrop::Job : InPsram {
    std::uint16_t *tinted = nullptr;  // three planes, sw by sh, each value times 256
    std::int32_t  *cols   = nullptr;
    std::uint8_t  *bgr    = nullptr;
    std::int32_t   sw = 0, sh = 0, w = 0, h = 0, r = 0;
    Cover::Made    made{};
    std::atomic<std::int32_t> done{0}, allowed{0};
    std::atomic<bool>         cancel{false}, running{true};
    std::uint64_t             began_us = 0, took_us = 0, work_us = 0;
#if !defined(ESP_PLATFORM)
    std::thread thread;
#endif
};

namespace {

using Made = Cover::Made;

constexpr std::int32_t STEP       = 8;    // the backdrop is worked out at an eighth and stretched up
constexpr std::int32_t ZOOM_PCT   = 135;  // closer in than filling, so it reads as colour rather than as the cover
constexpr std::int32_t BLUR_R     = 4;    // at the eighth, three passes: about 40 pixels on the screen
constexpr int          BLUR_PASSES = 3;
// What the PPA may take beside the picture it scales into: a big picture is
// scaled in software rather than have a second one's worth of memory found for it.
constexpr std::size_t   PPA_SCRATCH_MAX = 128 * 1024;
constexpr std::uint32_t RETRY_MS        = 1500;

char s_cover_report[128] = "no cover made yet\n";
char s_backdrop_report[192] = "no backdrop made yet";
double s_stretch_ms = -1, s_laid_ms = -1;
char s_report[sizeof s_cover_report + sizeof s_backdrop_report + 112];

struct Crop {
    std::int32_t x, y, w, h;
};

// The middle of the picture in the shape of w by h, `zoom_pct` percent closer.
Crop middle(const Picture &p, std::int32_t w, std::int32_t h, std::int32_t zoom_pct)
{
    std::int64_t cw = p.w, ch = static_cast<std::int64_t>(p.w) * h / w;
    if (ch > p.h) {
        ch = p.h;
        cw = static_cast<std::int64_t>(p.h) * w / h;
    }
    cw = std::max<std::int64_t>(1, cw * 100 / zoom_pct);
    ch = std::max<std::int64_t>(1, ch * 100 / zoom_pct);
    return {static_cast<std::int32_t>((p.w - cw) / 2), static_cast<std::int32_t>((p.h - ch) / 2),
            static_cast<std::int32_t>(cw), static_cast<std::int32_t>(ch)};
}

std::int32_t stride_of(const Picture &p) { return p.stride > 0 ? p.stride : p.w * 2; }

const std::uint16_t *row_of(const Picture &p, std::int32_t y)
{
    return reinterpret_cast<const std::uint16_t *>(static_cast<const std::uint8_t *>(p.pixels) +
                                                   static_cast<std::size_t>(y) * stride_of(p));
}

std::uint16_t *row_of(std::uint16_t *out, std::int32_t stride, std::int32_t y)
{
    return reinterpret_cast<std::uint16_t *>(reinterpret_cast<std::uint8_t *>(out) + static_cast<std::size_t>(y) * stride);
}

// Shrinking, a few points of each pixel's share of the picture averaged: near
// a box filter's look at a fraction of its reads. Growing, bilinear. False,
// with nothing written, when there is no room for its table of columns.
bool scale_soft(const Picture &p, Crop c, std::uint16_t *out, std::int32_t w, std::int32_t h, std::int32_t stride)
{
    if (c.w == w && c.h == h) {
        for (std::int32_t y = 0; y < h; ++y) {
            std::memcpy(row_of(out, stride, y), row_of(p, c.y + y) + c.x, static_cast<std::size_t>(w) * 2);
        }
        return true;
    }
    if (c.w >= w && c.h >= h) {
        const int n = std::clamp<int>(std::max((c.w + w - 1) / w, (c.h + h - 1) / h), 1, 4);
        auto *xs = static_cast<std::int32_t *>(lv_malloc(sizeof(std::int32_t) * w * n));
        if (xs == nullptr) {
            return false;
        }
        for (std::int32_t i = 0; i < w * n; ++i) {
            xs[i] = c.x + static_cast<std::int32_t>((2LL * i + 1) * c.w / (2LL * w * n));
        }
        const std::uint16_t *rows[4];
        for (std::int32_t y = 0; y < h; ++y) {
            for (int k = 0; k < n; ++k) {
                rows[k] = row_of(p, c.y + static_cast<std::int32_t>((2LL * (y * n + k) + 1) * c.h / (2LL * h * n)));
            }
            std::uint16_t *o = row_of(out, stride, y);
            for (std::int32_t x = 0; x < w; ++x) {
                int r = 0, g = 0, b = 0;
                for (int k = 0; k < n; ++k) {
                    for (int j = 0; j < n; ++j) {
                        const std::uint16_t v = rows[k][xs[x * n + j]];
                        r += v >> 11;
                        g += (v >> 5) & 0x3f;
                        b += v & 0x1f;
                    }
                }
                const int nn = n * n, half = nn / 2;
                o[x] = static_cast<std::uint16_t>(((r + half) / nn) << 11 | ((g + half) / nn) << 5 | (b + half) / nn);
            }
        }
        lv_free(xs);
        return true;
    }
    for (std::int32_t y = 0; y < h; ++y) {
        const std::int32_t fy  = std::max<std::int32_t>(0, static_cast<std::int32_t>(((2LL * y + 1) * c.h * 128) / h) - 128);
        const std::int32_t y0  = c.y + std::min(fy >> 8, c.h - 1), y1 = c.y + std::min((fy >> 8) + 1, c.h - 1);
        const std::int32_t ty  = fy & 0xff;
        const std::uint16_t *a = row_of(p, y0), *b = row_of(p, y1);
        std::uint16_t       *o = row_of(out, stride, y);
        for (std::int32_t x = 0; x < w; ++x) {
            const std::int32_t fx = std::max<std::int32_t>(0, static_cast<std::int32_t>(((2LL * x + 1) * c.w * 128) / w) - 128);
            const std::int32_t x0 = c.x + std::min(fx >> 8, c.w - 1), x1 = c.x + std::min((fx >> 8) + 1, c.w - 1);
            const std::int32_t tx = fx & 0xff;
            const std::uint16_t q[4] = {a[x0], a[x1], b[x0], b[x1]};
            const auto mix = [&](int shift, int mask) {
                const int top = ((q[0] >> shift) & mask) * (256 - tx) + ((q[1] >> shift) & mask) * tx;
                const int bot = ((q[2] >> shift) & mask) * (256 - tx) + ((q[3] >> shift) & mask) * tx;
                return (top * (256 - ty) + bot * ty + 32768) >> 16;
            };
            o[x] = static_cast<std::uint16_t>(mix(11, 0x1f) << 11 | mix(5, 0x3f) << 5 | mix(0, 0x1f));
        }
    }
    return true;
}

#if defined(ESP_PLATFORM)
ppa_client_handle_t s_ppa = nullptr;

// The PPA scales in sixteenths: the crop is narrowed until a whole number of
// them makes at least w by h, which is then cut out of the middle.
bool scale_ppa(const Picture &p, Crop c, std::uint16_t *out, std::int32_t w, std::int32_t h, std::int32_t stride)
{
    if (s_ppa == nullptr) {
        ppa_client_config_t client{};
        client.oper_type             = PPA_OPERATION_SRM;
        client.max_pending_trans_num = 1;
        if (ppa_register_client(&client, &s_ppa) != ESP_OK) {
            return false;
        }
    }
    const std::int32_t qx = std::clamp<std::int32_t>((16 * w + c.w - 1) / c.w, 1, 255);
    const std::int32_t qy = std::clamp<std::int32_t>((16 * h + c.h - 1) / c.h, 1, 255);
    const std::int32_t cw = std::min(c.w, (16 * w + qx - 1) / qx), ch = std::min(c.h, (16 * h + qy - 1) / qy);
    const std::int32_t ow = cw * qx / 16, oh = ch * qy / 16;
    if (ow < w || oh < h) {
        return false;
    }
    const std::size_t bytes = (static_cast<std::size_t>(ow) * oh * 2 + 127) / 128 * 128;
    if (bytes > PPA_SCRATCH_MAX) {
        return false;
    }
    auto *scratch = static_cast<std::uint16_t *>(heap_caps_aligned_calloc(128, 1, bytes, MALLOC_CAP_SPIRAM));
    if (scratch == nullptr) {
        return false;
    }
    ppa_srm_oper_config_t op{};
    op.in.buffer         = p.pixels;
    op.in.pic_w          = static_cast<std::uint32_t>(stride_of(p) / 2);
    op.in.pic_h          = static_cast<std::uint32_t>(p.h);
    op.in.block_w        = static_cast<std::uint32_t>(cw);
    op.in.block_h        = static_cast<std::uint32_t>(ch);
    op.in.block_offset_x = static_cast<std::uint32_t>(c.x + (c.w - cw) / 2);
    op.in.block_offset_y = static_cast<std::uint32_t>(c.y + (c.h - ch) / 2);
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer        = scratch;
    op.out.buffer_size   = bytes;
    op.out.pic_w         = static_cast<std::uint32_t>(ow);
    op.out.pic_h         = static_cast<std::uint32_t>(oh);
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x           = static_cast<float>(qx) / 16.0f;
    op.scale_y           = static_cast<float>(qy) / 16.0f;
    op.mode              = PPA_TRANS_MODE_BLOCKING;
    const bool ok        = ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
    if (ok) {
        const std::int32_t dx = (ow - w) / 2, dy = (oh - h) / 2;
        for (std::int32_t y = 0; y < h; ++y) {
            std::memcpy(row_of(out, stride, y), scratch + static_cast<std::size_t>(y + dy) * ow + dx, static_cast<std::size_t>(w) * 2);
        }
    }
    heap_caps_free(scratch);
    return ok;
}
#endif

/**
 * The middle of `p` scaled to fill w by h: the PPA on the panel, and software
 * where there is none, it refuses, or it would need too much beside. False,
 * with nothing written, when neither could; `hw` says the PPA did it.
 */
bool scale(const Picture &p, Crop c, std::uint16_t *out, std::int32_t w, std::int32_t h, std::int32_t stride, bool &hw)
{
    hw = false;
#if defined(ESP_PLATFORM)
    if ((c.w != w || c.h != h) && scale_ppa(p, c, out, w, h, stride)) {
        hw = true;
        return true;
    }
#endif
    return scale_soft(p, c, out, w, h, stride);
}

lv_opa_t coverage(std::int32_t x, std::int32_t y, float w, float h, float r)
{
    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
    const float cx = std::clamp(px, r, w - r);
    const float cy = std::clamp(py, r, h - r);
    const float d  = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    return static_cast<lv_opa_t>(std::lround(std::clamp(r - d + 0.5f, 0.0f, 1.0f) * 255.0f));
}

bool all_corner(std::int32_t w, std::int32_t h, std::int32_t r) { return r > 0 && 2 * r >= std::min(w, h) - 2; }

lv_opa_t *alpha_of(lv_draw_buf_t *buf) { return buf->data + buf->header.stride * buf->header.h; }

void destroy(lv_draw_buf_t *&buf)
{
    if (buf != nullptr) {
        lv_image_cache_drop(buf);
        lv_draw_buf_destroy(buf);
        buf = nullptr;
    }
}

void destroy(Made &m)
{
    destroy(m.main);
    for (lv_draw_buf_t *&c : m.corner) {
        destroy(c);
    }
}

// What allocate() asks for, near enough to tell in a log.
std::size_t bytes_for(std::int32_t w, std::int32_t h, std::int32_t r)
{
    const std::size_t px = static_cast<std::size_t>(w) * h;
    return all_corner(w, h, r) ? px * 3 : px * 2 + 4 * static_cast<std::size_t>(r) * r * 3;
}

void no_room(bool &warned, const char *what, std::int32_t w, std::int32_t h, std::size_t asked)
{
    if (std::exchange(warned, true)) {
        return;
    }
    ESP_LOGW("kit", "no room for a %dx%d %s: %u bytes asked, the largest free PSRAM block %u", static_cast<int>(w),
             static_cast<int>(h), what, static_cast<unsigned>(asked),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

std::size_t bytes_of(const Made &m)
{
    std::size_t n = m.main != nullptr ? m.main->data_size : 0;
    for (const lv_draw_buf_t *c : m.corner) {
        n += c != nullptr ? c->data_size : 0;
    }
    return n;
}

// A picture as it is drawn: opaque in the middle, and its corners, which alone
// carry an alpha, apart. A circle is all corner, and alpha through.
bool allocate(Made &m, std::int32_t w, std::int32_t h, std::int32_t r)
{
    const bool round = all_corner(w, h, r);
    m.main = lv_draw_buf_create(w, h, round ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (m.main == nullptr) {
        return false;
    }
    if (round) {
        lv_opa_t *a = alpha_of(m.main);
        const std::int32_t as = m.main->header.stride / 2;
        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                a[y * as + x] = coverage(x, y, w, h, r);
            }
        }
        return true;
    }
    for (int k = 0; r > 0 && k < 4; ++k) {
        lv_draw_buf_t *c = m.corner[k] = lv_draw_buf_create(r, r, LV_COLOR_FORMAT_RGB565A8, LV_STRIDE_AUTO);
        if (c == nullptr) {
            destroy(m);
            return false;
        }
        lv_opa_t *a = alpha_of(c);
        const std::int32_t as = c->header.stride / 2, ox = k & 1 ? w - r : 0, oy = k & 2 ? h - r : 0;
        for (std::int32_t y = 0; y < r; ++y) {
            for (std::int32_t x = 0; x < r; ++x) {
                a[y * as + x] = coverage(ox + x, oy + y, w, h, r);
            }
        }
    }
    return true;
}

// The corners' colours, from the rows of the main picture from y0 to y1.
void corners_from_main(Made &m, std::int32_t w, std::int32_t h, std::int32_t r, std::int32_t y0, std::int32_t y1)
{
    if (m.corner[0] == nullptr) {
        return;
    }
    const std::int32_t ms = m.main->header.stride;
    for (int k = 0; k < 4; ++k) {
        lv_draw_buf_t     *c  = m.corner[k];
        const std::int32_t oy = k & 2 ? h - r : 0, ox = k & 1 ? w - r : 0;
        for (std::int32_t y = std::max(y0, oy); y < std::min(y1, oy + r); ++y) {
            std::memcpy(c->data + (y - oy) * c->header.stride, m.main->data + y * ms + ox * 2, static_cast<std::size_t>(r) * 2);
        }
    }
}

void lay(lv_layer_t *layer, const lv_draw_buf_t *buf, const lv_area_t &at, const lv_area_t &part, lv_opa_t opa)
{
    lv_area_t clip;
    if (!lv_area_intersect(&clip, &part, &layer->_clip_area)) {
        return;
    }
    const lv_area_t was = layer->_clip_area;
    layer->_clip_area   = clip;
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = buf;
    dsc.opa = opa;
    lv_draw_image(layer, &dsc, &at);
    layer->_clip_area = was;
}

// The opaque middle as up to three plain copies, which the PPA takes on the
// panel, and the four corners blended over what is under them.
void draw(const Made &m, lv_layer_t *layer, const lv_area_t &c, std::int32_t r, lv_opa_t opa, std::int32_t rows)
{
    lv_area_t valid = c;
    valid.y2        = std::min(c.y2, c.y1 + rows - 1);
    if (m.main == nullptr || valid.y2 < valid.y1 || opa <= LV_OPA_MIN) {
        return;
    }
    if (m.corner[0] == nullptr) {
        lay(layer, m.main, c, valid, opa);
        return;
    }
    const lv_area_t parts[3] = {{c.x1 + r, c.y1, c.x2 - r, c.y2},
                                {c.x1, c.y1 + r, c.x1 + r - 1, c.y2 - r},
                                {c.x2 - r + 1, c.y1 + r, c.x2, c.y2 - r}};
    for (const lv_area_t &part : parts) {
        lv_area_t p;
        if (lv_area_intersect(&p, &part, &valid)) {
            lay(layer, m.main, c, p, opa);
        }
    }
    for (int k = 0; k < 4; ++k) {
        const std::int32_t x = k & 1 ? c.x2 - r + 1 : c.x1, y = k & 2 ? c.y2 - r + 1 : c.y1;
        const lv_area_t    a{x, y, x + r - 1, y + r - 1};
        lv_area_t          p;
        if (lv_area_intersect(&p, &a, &valid)) {
            lay(layer, m.corner[k], a, p, opa);
        }
    }
}

// The copy of the picture a new one fades in over: whole for a small one, at a
// half or a quarter for bigger ones, which only show it blurred and going.
std::int32_t ghost_step(std::int32_t w, std::int32_t h)
{
    const std::int32_t side = std::max(w, h);
    return side <= 64 ? 1 : side <= 200 ? 2 : 4;
}

void lay_ghost(lv_layer_t *layer, lv_draw_buf_t *ghost, const lv_area_t &c)
{
    lv_area_t clip;
    if (!lv_area_intersect(&clip, &c, &layer->_clip_area)) {
        return;
    }
    const std::int32_t bw = ghost->header.w, bh = ghost->header.h, w = lv_area_get_width(&c), h = lv_area_get_height(&c);
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = ghost;
    if (bw != w || bh != h) {
        const std::int32_t gw = bw - 1, gh = bh - 1;  // without the edge kept once more
        dsc.scale_x   = (w * LV_SCALE_NONE + gw / 2) / gw;
        dsc.scale_y   = (h * LV_SCALE_NONE + gh / 2) / gh;
        dsc.pivot     = {0, 0};
        dsc.antialias = 1;
    }
    const lv_area_t at{c.x1, c.y1, c.x1 + bw - 1, c.y1 + bh - 1};
    const lv_area_t was = layer->_clip_area;
    layer->_clip_area   = clip;
    lv_draw_image(layer, &dsc, &at);
    layer->_clip_area = was;
}

// What surely lies inside the shape, so whatever is under it is not drawn
// there, and the rest round it, where it is: invalidating the whole box would
// make LVGL draw under all of it, as a box with round corners never covers it.
void invalidate(lv_obj_t *obj, std::int32_t r)
{
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    const std::int32_t w = lv_area_get_width(&c), h = lv_area_get_height(&c);
    if (r <= 0) {
        lv_obj_invalidate(obj);
        return;
    }
    if (all_corner(w, h, r)) {
        const std::int32_t mx = (w - w * 707 / 1000) / 2 + 1, my = (h - h * 707 / 1000) / 2 + 1;
        const lv_area_t parts[5] = {{c.x1 + mx, c.y1 + my, c.x2 - mx, c.y2 - my},
                                    {c.x1, c.y1, c.x2, c.y1 + my - 1},
                                    {c.x1, c.y2 - my + 1, c.x2, c.y2},
                                    {c.x1, c.y1 + my, c.x1 + mx - 1, c.y2 - my},
                                    {c.x2 - mx + 1, c.y1 + my, c.x2, c.y2 - my}};
        for (const lv_area_t &a : parts) {
            lv_obj_invalidate_area(obj, &a);
        }
        return;
    }
    const lv_area_t parts[7] = {{c.x1 + r, c.y1, c.x2 - r, c.y2},
                                {c.x1, c.y1 + r, c.x1 + r - 1, c.y2 - r},
                                {c.x2 - r + 1, c.y1 + r, c.x2, c.y2 - r},
                                {c.x1, c.y1, c.x1 + r - 1, c.y1 + r - 1},
                                {c.x2 - r + 1, c.y1, c.x2, c.y1 + r - 1},
                                {c.x1, c.y2 - r + 1, c.x1 + r - 1, c.y2},
                                {c.x2 - r + 1, c.y2 - r + 1, c.x2, c.y2}};
    for (const lv_area_t &a : parts) {
        lv_obj_invalidate_area(obj, &a);
    }
}

void cover_check(lv_event_t *e, std::int32_t r, bool opaque)
{
    auto *info = static_cast<lv_cover_check_info_t *>(lv_event_get_param(e));
    if (info->res == LV_COVER_RES_MASKED) {
        return;
    }
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &c);
    info->res = opaque && lv_area_is_in(info->area, &c, r) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
}

double ms(std::uint64_t us) { return static_cast<double>(us) / 1000.0; }
double kb(std::size_t bytes) { return static_cast<double>(bytes) / 1024.0; }

// --- the backdrop's own steps ------------------------------------------------

// Three box blurs make nearly a Gaussian one; each runs along a line on a copy of it.
void box_line(std::uint16_t *p, std::int32_t n, std::int32_t step, std::uint16_t *line)
{
    for (std::int32_t i = 0; i < n; ++i) {
        line[i] = p[i * step];
    }
    const std::int32_t span = 2 * BLUR_R + 1;
    std::int32_t sum = 0;
    for (std::int32_t k = -BLUR_R; k <= BLUR_R; ++k) {
        sum += line[std::clamp<std::int32_t>(k, 0, n - 1)];
    }
    for (std::int32_t i = 0; i < n; ++i) {
        p[i * step] = static_cast<std::uint16_t>(sum / span);
        sum += line[std::min<std::int32_t>(i + BLUR_R + 1, n - 1)] - line[std::max<std::int32_t>(i - BLUR_R, 0)];
    }
}

void blur(std::uint16_t *planes, std::int32_t sw, std::int32_t sh, std::uint16_t *line)
{
    for (int c = 0; c < 3; ++c) {
        std::uint16_t *p = planes + c * sw * sh;
        for (int pass = 0; pass < BLUR_PASSES; ++pass) {
            for (std::int32_t y = 0; y < sh; ++y) {
                box_line(p + y * sw, sw, 1, line);
            }
            for (std::int32_t x = 0; x < sw; ++x) {
                box_line(p + x, sh, sw, line);
            }
        }
    }
}

// Darker and a little more vivid in the dark look, so light ink reads on it at
// any cover; pale in the light one, for dark ink. Both lean to the ground.
void tint(const std::uint16_t *in, std::uint16_t *out, std::int32_t n)
{
    const bool          dark = look() == Look::Dark;
    const std::uint32_t g    = colours().ground;
    const float ground[3] = {static_cast<float>((g >> 16) & 0xff) / 255.0f, static_cast<float>((g >> 8) & 0xff) / 255.0f,
                             static_cast<float>(g & 0xff) / 255.0f};
    for (std::int32_t i = 0; i < n; ++i) {
        float c[3];
        for (int k = 0; k < 3; ++k) {
            c[k] = static_cast<float>(in[k * n + i]) / 65535.0f;
        }
        const float l = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
        for (float &v : c) {
            v = std::clamp(l + (v - l) * 1.3f, 0.0f, 1.0f);
        }
        if (dark) {
            for (int k = 0; k < 3; ++k) {
                c[k] = c[k] * 0.6f + (ground[k] - c[k] * 0.6f) * 0.22f;
            }
            const float lum = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
            if (lum > 0.22f) {
                for (float &v : c) {
                    v *= 0.22f / lum;
                }
            }
        } else {
            for (int k = 0; k < 3; ++k) {
                c[k] += (ground[k] - c[k]) * 0.55f;
            }
            const float lum = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
            if (lum < 0.8f) {
                const float t = (0.8f - lum) / (1.0f - lum);
                for (float &v : c) {
                    v += (1.0f - v) * t;
                }
            }
        }
        for (int k = 0; k < 3; ++k) {
            out[k * n + i] = static_cast<std::uint16_t>(std::clamp(c[k], 0.0f, 1.0f) * 65535.0f + 0.5f);
        }
    }
}

const pixels::Ditherer &ditherer()
{
    static const auto *d = new pixels::Ditherer();
    return *d;
}

// Off the LVGL task: the tinted eighth stretched up, bilinear and dithered, a
// row at a time and never more rows ahead than it has been let.
void stretch(Backdrop::Job *job)
{
    const std::int32_t sw = job->sw, sh = job->sh, w = job->w, h = job->h, r = job->r, n = sw * sh;
    const pixels::Ditherer &dither = ditherer();
    lv_draw_buf_t *main = job->made.main;
    std::int32_t   flushed = 0;
    for (std::int32_t y = 0; y < h; ++y) {
        while (y >= job->allowed.load(std::memory_order_acquire) && !job->cancel.load(std::memory_order_relaxed)) {
#if defined(ESP_PLATFORM)
            vTaskDelay(1);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
        }
        if (job->cancel.load(std::memory_order_relaxed)) {
            break;
        }
        const std::uint64_t row_at = perf::now_us();
        const std::int32_t fy = y % STEP, j0 = y / STEP, j1 = std::min(j0 + 1, sh - 1);
        for (int c = 0; c < 3; ++c) {
            const std::uint16_t *t = job->tinted + c * n;
            std::int32_t        *col = job->cols + c * sw;
            for (std::int32_t i = 0; i < sw; ++i) {
                col[i] = t[j0 * sw + i] * (STEP - fy) + t[j1 * sw + i] * fy;
            }
        }
        for (int c = 0; c < 3; ++c) {
            const std::int32_t *col = job->cols + c * sw;
            std::uint8_t       *o   = job->bgr + (2 - c);
            for (std::int32_t i = 0; i * STEP < w; ++i) {
                std::int32_t       v  = col[i] * STEP;
                const std::int32_t dv = col[std::min(i + 1, sw - 1)] - col[i];
                for (std::int32_t x = i * STEP; x < std::min(w, (i + 1) * STEP); ++x, v += dv) {
                    o[x * 3] = static_cast<std::uint8_t>(v >> 14);
                }
            }
        }
        dither.row(job->bgr, reinterpret_cast<std::uint16_t *>(main->data + y * main->header.stride), w, y);
        if (y < r || y >= h - r) {
            corners_from_main(job->made, w, h, r, y, y + 1);
        }
        job->work_us += perf::now_us() - row_at;
        if ((y + 1) % STEP == 0 || y + 1 == h) {
            const lv_area_t rows{0, flushed, w - 1, y};
            lv_draw_buf_flush_cache(main, &rows);
            flushed = y + 1;
            job->done.store(y + 1, std::memory_order_release);
        }
    }
    job->took_us = perf::now_us() - job->began_us;
    job->running.store(false, std::memory_order_release);
}

#if defined(ESP_PLATFORM)
void stretch_task(void *arg)
{
    stretch(static_cast<Backdrop::Job *>(arg));
    vTaskDelete(nullptr);
}
#endif

void free_job(Backdrop::Job *job)
{
    lv_free(job->tinted);
    lv_free(job->cols);
    lv_free(job->bgr);
    delete job;
}

}  // namespace

const char *pictures_report()
{
    char stretched[112] = "";
    if (s_stretch_ms >= 0) {
        std::snprintf(stretched, sizeof stretched, ", stretched and dithered off the LVGL task in %.1f ms, laid down over %.0f ms",
                      s_stretch_ms, s_laid_ms);
    }
    std::snprintf(s_report, sizeof s_report, "%s%s%s\n", s_cover_report, s_backdrop_report, stretched);
    return s_report;
}

// --- cover ----------------------------------------------------------------

Cover::Cover(lv_obj_t *parent, std::int32_t w, std::int32_t h, Shape shape, std::int32_t radius, const char *glyph)
    : m_w(w), m_h(h)
{
    const std::int32_t side = std::min(w, h);
    m_r = shape == Shape::Round ? side / 2 : radius >= 0 ? std::min(radius, side / 2) : side >= 100 ? 16 : side >= 48 ? 12 : 8;
    m_root = lv_obj_create(parent);
    lv_obj_remove_style_all(m_root);
    lv_obj_set_size(m_root, w, h);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_CLICKABLE);

    m_well = well(m_root);
    lv_obj_set_size(m_well, w, h);
    lv_obj_set_style_radius(m_well, shape == Shape::Round ? LV_RADIUS_CIRCLE : m_r, 0);
    lv_obj_set_style_pad_all(m_well, 0, 0);
    lv_obj_remove_flag(m_well, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *mark = icon(m_well, glyph != nullptr ? glyph : glyph::music_note, side >= 96 ? 34 : side >= 56 ? 28 : 24, false,
                          Ink::Tertiary);
    lv_obj_center(mark);

    m_pic = lv_obj_create(m_root);
    lv_obj_remove_style_all(m_pic);
    lv_obj_set_size(m_pic, w, h);
    lv_obj_remove_flag(m_pic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(m_pic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(m_pic, on_draw, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(m_pic, on_draw, LV_EVENT_COVER_CHECK, this);
    lv_obj_add_event_cb(
        m_root,
        [](lv_event_t *e) {
            auto *c = static_cast<Cover *>(lv_event_get_user_data(e));
            lv_anim_delete(c, on_mix);
            destroy(c->m_cur);
            destroy(c->m_ghost);
            c->m_root = c->m_well = c->m_pic = nullptr;
        },
        LV_EVENT_DELETE, this);
}

std::size_t Cover::bytes() const { return bytes_of(m_cur) + (m_ghost != nullptr ? m_ghost->data_size : 0); }

void Cover::on_draw(lv_event_t *e)
{
    auto *c = static_cast<Cover *>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_COVER_CHECK) {
        cover_check(e, c->m_r, c->m_has && c->m_fade == Fade::None);
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(c->m_pic, &at);
    lv_layer_t   *layer = lv_event_get_layer(e);
    const auto    mix   = static_cast<lv_opa_t>(c->m_mix);
    switch (c->m_fade) {
    case Fade::None:
        if (c->m_has) {
            draw(c->m_cur, layer, at, c->m_r, LV_OPA_COVER, c->m_h);
        }
        break;
    case Fade::Over:
        lay_ghost(layer, c->m_ghost, at);
        draw(c->m_cur, layer, at, c->m_r, mix, c->m_h);
        break;
    case Fade::In: draw(c->m_cur, layer, at, c->m_r, mix, c->m_h); break;
    case Fade::Out: draw(c->m_cur, layer, at, c->m_r, static_cast<lv_opa_t>(255 - mix), c->m_h); break;
    }
}

void Cover::on_mix(void *var, int32_t v)
{
    auto *c = static_cast<Cover *>(var);
    if (v != c->m_mix) {
        c->m_mix = v;
        invalidate(c->m_pic, c->m_r);
    }
}

void Cover::settle()
{
    m_fade = Fade::None;
    m_mix  = 255;
    lv_obj_set_hidden(m_well, m_has);
    invalidate(m_pic, m_r);
}

void Cover::fade(Fade kind)
{
    lv_anim_delete(this, on_mix);
    if (kind == Fade::None) {
        settle();
        return;
    }
    m_fade = kind;
    m_mix  = 0;
    lv_obj_set_hidden(m_well, kind == Fade::Over);
    invalidate(m_pic, m_r);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_exec_cb(&a, on_mix);
    lv_anim_set_duration(&a, motion::fade_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, [](lv_anim_t *an) { static_cast<Cover *>(an->var)->settle(); });
    lv_anim_start(&a);
}

// What is shown now, shrunk into the ghost, for the next one to fade in over:
// the picture itself is written over in its place. A shrunk ghost has its last
// column and row once more, as stretching it fades its far edges into what
// lies beyond them.
bool Cover::keep_ghost()
{
    const std::int32_t step = ghost_step(m_w, m_h), gw = (m_w + step - 1) / step, gh = (m_h + step - 1) / step;
    const std::int32_t pad = step > 1 ? 1 : 0, bw = gw + pad, bh = gh + pad;
    if (m_ghost == nullptr) {
        m_ghost = lv_draw_buf_create(bw, bh, m_r > 0 ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        if (m_ghost == nullptr) {
            return false;
        }
        if (m_r > 0) {
            lv_opa_t          *a  = alpha_of(m_ghost);
            const std::int32_t as = m_ghost->header.stride / 2;
            const float        r  = static_cast<float>(m_r) * gw / m_w;
            for (std::int32_t y = 0; y < bh; ++y) {
                for (std::int32_t x = 0; x < bw; ++x) {
                    a[y * as + x] = coverage(std::min(x, gw - 1), std::min(y, gh - 1), gw, gh, r);
                }
            }
        }
    }
    const std::int32_t stride = static_cast<std::int32_t>(m_ghost->header.stride);
    auto              *out    = reinterpret_cast<std::uint16_t *>(m_ghost->data);
    const Picture      shown{m_cur.main->data, m_w, m_h, static_cast<std::int32_t>(m_cur.main->header.stride)};
    if (!scale_soft(shown, {0, 0, m_w, m_h}, out, gw, gh, stride)) {
        return false;
    }
    if (pad > 0) {
        for (std::int32_t y = 0; y < gh; ++y) {
            row_of(out, stride, y)[gw] = row_of(out, stride, y)[gw - 1];
        }
        std::memcpy(row_of(out, stride, gh), row_of(out, stride, gh - 1), static_cast<std::size_t>(bw) * 2);
    }
    lv_draw_buf_flush_cache(m_ghost, nullptr);
    lv_image_cache_drop(m_ghost);
    return true;
}

bool Cover::show(const Picture &picture, bool animate)
{
    if (m_root == nullptr || picture.pixels == nullptr || picture.w <= 0 || picture.h <= 0) {
        return false;
    }
    const std::uint64_t t0 = perf::now_us();
    lv_anim_delete(this, on_mix);
    settle();
    if (m_cur.main == nullptr && !allocate(m_cur, m_w, m_h, m_r)) {
        no_room(m_warned, "picture", m_w, m_h, bytes_for(m_w, m_h, m_r));
        return false;
    }
    const bool over = animate && m_has && keep_ghost();
    bool       hw   = false;
    if (!scale(picture, middle(picture, m_w, m_h, 100), reinterpret_cast<std::uint16_t *>(m_cur.main->data), m_w, m_h,
               static_cast<std::int32_t>(m_cur.main->header.stride), hw)) {
        no_room(m_warned, "picture's scaling", m_w, m_h, sizeof(std::int32_t) * 4 * m_w);
        return false;
    }
    corners_from_main(m_cur, m_w, m_h, m_r, 0, m_h);
    lv_draw_buf_flush_cache(m_cur.main, nullptr);
    lv_image_cache_drop(m_cur.main);
    for (lv_draw_buf_t *c : m_cur.corner) {
        if (c != nullptr) {
            lv_image_cache_drop(c);
        }
    }
    const Fade kind = !animate ? Fade::None : over ? Fade::Over : m_has ? Fade::None : Fade::In;
    m_has    = true;
    m_warned = false;
    fade(kind);
    std::snprintf(s_cover_report, sizeof s_cover_report, "cover %dx%d%s: %.1f KB, made in %.2f ms%s\n",
                  static_cast<int>(m_w), static_cast<int>(m_h), all_corner(m_w, m_h, m_r) ? " round" : "",
                  kb(bytes()), ms(perf::now_us() - t0), hw ? " (PPA)" : "");
    return true;
}

void Cover::clear(bool animate)
{
    if (m_root == nullptr || !m_has) {
        return;
    }
    lv_anim_delete(this, on_mix);
    settle();
    m_has = false;
    fade(animate ? Fade::Out : Fade::None);
}

void Cover::release()
{
    if (m_root == nullptr) {
        return;
    }
    lv_anim_delete(this, on_mix);
    destroy(m_cur);
    destroy(m_ghost);
    m_has = false;
    settle();
}

// --- backdrop -------------------------------------------------------------

Backdrop::Backdrop(lv_obj_t *parent, std::int32_t w, std::int32_t h, std::int32_t radius)
    : m_w(w), m_h(h), m_r(std::min(radius, std::min(w, h) / 2))
{
    m_on_ground = w == size::screen_w && h == size::screen_h && m_r == 0;
    m_sw   = (w - 1) / STEP + 2;
    m_sh   = (h - 1) / STEP + 2;
    m_root = lv_obj_create(parent);
    lv_obj_remove_style_all(m_root);
    lv_obj_set_size(m_root, w, h);
    lv_obj_set_style_radius(m_root, m_r, 0);
    lv_obj_set_style_bg_opa(m_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(m_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m_root, on_draw, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(m_root, on_cover, LV_EVENT_COVER_CHECK, this);
    lv_obj_add_event_cb(
        m_root,
        [](lv_event_t *e) {
            auto *b = static_cast<Backdrop *>(lv_event_get_user_data(e));
            b->stop();
            b->give_back();
            lv_free(b->m_small);
            b->m_small = nullptr;
            b->m_root  = nullptr;
        },
        LV_EVENT_DELETE, this);
    restyle();
    on_restyle(m_root, [](void *ctx) { static_cast<Backdrop *>(ctx)->restyle(); }, this);
}

std::size_t Backdrop::bytes() const
{
    std::size_t n = (m_lent ? 0 : bytes_of(m_made)) + (m_small != nullptr ? sizeof(std::uint16_t) * 3 * m_sw * m_sh : 0);
    return n + (m_job != nullptr ? sizeof(std::uint16_t) * 3 * m_sw * m_sh : 0);
}

void Backdrop::on_draw(lv_event_t *e)
{
    auto *b = static_cast<Backdrop *>(lv_event_get_user_data(e));
    lv_area_t at;
    lv_obj_get_coords(b->m_root, &at);
    draw(b->m_made, lv_event_get_layer(e), at, b->m_r, LV_OPA_COVER, b->m_whole ? b->m_h : b->m_shown);
}

void Backdrop::on_cover(lv_event_t *e)
{
    auto *b = static_cast<Backdrop *>(lv_event_get_user_data(e));
    if (b->m_whole) {
        cover_check(e, b->m_r, true);
    }
}

void Backdrop::restyle()
{
    lv_obj_set_style_bg_color(m_root, lv_color_hex(colours().ground), 0);
    if (m_small != nullptr) {
        start();
    }
}

bool Backdrop::show(const Picture &picture)
{
    if (m_root == nullptr || picture.pixels == nullptr || picture.w <= 0 || picture.h <= 0) {
        return false;
    }
    const std::uint64_t t0 = perf::now_us();
    stop();
    const std::int32_t n = m_sw * m_sh;
    if (m_small == nullptr) {
        m_small = static_cast<std::uint16_t *>(lv_malloc(sizeof(std::uint16_t) * 3 * n));
    }
    auto *shrunk = static_cast<std::uint16_t *>(lv_malloc(sizeof(std::uint16_t) * n));
    auto *line   = static_cast<std::uint16_t *>(lv_malloc(sizeof(std::uint16_t) * std::max(m_sw, m_sh)));
    bool hw = false;
    if (m_small == nullptr || shrunk == nullptr || line == nullptr ||
        !scale(picture, middle(picture, m_w, m_h, ZOOM_PCT), shrunk, m_sw, m_sh, m_sw * 2, hw)) {
        lv_free(shrunk);
        lv_free(line);
        no_room(m_warned, "backdrop's small picture", m_sw, m_sh, sizeof(std::uint16_t) * 4 * n);
        clear();
        return false;
    }
    for (std::int32_t i = 0; i < n; ++i) {
        const std::uint16_t v = shrunk[i];
        m_small[i]         = static_cast<std::uint16_t>((v >> 11) * 65535 / 31);
        m_small[n + i]     = static_cast<std::uint16_t>(((v >> 5) & 0x3f) * 65535 / 63);
        m_small[2 * n + i] = static_cast<std::uint16_t>((v & 0x1f) * 65535 / 31);
    }
    lv_free(shrunk);
    blur(m_small, m_sw, m_sh, line);
    lv_free(line);
    const std::uint64_t small_us = perf::now_us() - t0;
    if (!start()) {
        return false;
    }
    m_warned = false;
    std::snprintf(s_backdrop_report, sizeof s_backdrop_report,
                  "backdrop %dx%d%s: %.1f KB, %.1f KB of it kept to remake it in another look; shrunk and blurred in %.2f ms%s",
                  static_cast<int>(m_w), static_cast<int>(m_h), m_lent ? " in the ground's picture" : "", kb(bytes()),
                  kb(sizeof(std::uint16_t) * 3 * n), ms(small_us), hw ? " (PPA)" : "");
    return true;
}

void Backdrop::give_back()
{
    if (m_lent) {
        m_made.main = nullptr;
        m_lent      = false;
        ground_back();
    } else {
        destroy(m_made);
    }
}

// False, and the plain ground, when there is nothing to make it into or no
// room to make it, so the caller tries again later.
bool Backdrop::start()
{
    stop();
    if (m_made.main == nullptr) {
        if (m_on_ground) {
            m_made.main = lend_ground();
            m_lent      = m_made.main != nullptr;
        } else {
            allocate(m_made, m_w, m_h, m_r);
        }
    }
    if (m_made.main == nullptr) {
        no_room(m_warned, m_on_ground ? "backdrop, the ground's picture lent already," : "backdrop", m_w, m_h,
                m_on_ground ? 0 : bytes_for(m_w, m_h, m_r));
        clear();
        return false;
    }
    const std::int32_t n = m_sw * m_sh;
    auto *job          = new Job;
    job->tinted        = static_cast<std::uint16_t *>(lv_malloc(sizeof(std::uint16_t) * 3 * n));
    job->cols          = static_cast<std::int32_t *>(lv_malloc(sizeof(std::int32_t) * 3 * m_sw));
    job->bgr           = static_cast<std::uint8_t *>(lv_malloc(static_cast<std::size_t>(m_w) * 3 + 16));
    if (job->tinted == nullptr || job->cols == nullptr || job->bgr == nullptr) {
        free_job(job);
        no_room(m_warned, "backdrop's tinting", m_sw, m_sh, sizeof(std::uint16_t) * 3 * n);
        clear();
        return false;
    }
    tint(m_small, job->tinted, n);
    job->sw = m_sw, job->sh = m_sh, job->w = m_w, job->h = m_h, job->r = m_r;
    job->made     = m_made;
    job->began_us = perf::now_us();
    job->allowed.store(std::max<std::int32_t>(STEP, m_h / 8), std::memory_order_release);
    m_job   = job;
    m_pass = 0;
#if defined(ESP_PLATFORM)
    // core 0, away from LVGL's
    if (xTaskCreatePinnedToCore(stretch_task, "backdrop", 4096, job, tskIDLE_PRIORITY + 2, nullptr, 0) != pdPASS) {
        m_job = nullptr;
        free_job(job);
        clear();
        return false;
    }
#else
    job->thread = std::thread(stretch, job);
#endif
    m_timer = lv_timer_create(on_tick, 15, this);
    return true;
}

// Each tick lays down the rows made since the last, and lets the maker one
// band ahead: the band being written is never one LVGL is about to draw.
void Backdrop::on_tick(lv_timer_t *t)
{
    auto *b   = static_cast<Backdrop *>(lv_timer_get_user_data(t));
    Job  *job = b->m_job;
    const std::int32_t done = job->done.load(std::memory_order_acquire);
    if (done > b->m_pass) {
        lv_area_t c;
        lv_obj_get_coords(b->m_root, &c);
        const lv_area_t rows{c.x1, c.y1 + b->m_pass, c.x2, c.y1 + done - 1};
        lv_obj_invalidate_area(b->m_root, &rows);
        b->m_pass  = done;
        b->m_shown = std::max(b->m_shown, done);
        job->allowed.store(std::min(b->m_h, done + std::max<std::int32_t>(STEP, b->m_h / 8)), std::memory_order_release);
    }
    if (done < b->m_h || job->running.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t took = job->took_us, work = job->work_us;
    b->stop();
    if (!b->m_whole) {
        b->m_whole = true;
        lv_obj_set_style_bg_opa(b->m_root, LV_OPA_TRANSP, 0);
    }
    s_stretch_ms = ms(work);
    s_laid_ms    = ms(took);
}

void Backdrop::stop()
{
    if (m_timer != nullptr) {
        lv_timer_delete(m_timer);
        m_timer = nullptr;
    }
    if (m_job == nullptr) {
        return;
    }
    m_job->cancel.store(true, std::memory_order_relaxed);
#if defined(ESP_PLATFORM)
    while (m_job->running.load(std::memory_order_acquire)) {
        vTaskDelay(1);
    }
#else
    m_job->thread.join();
#endif
    free_job(m_job);
    m_job = nullptr;
}

void Backdrop::clear()
{
    if (m_root == nullptr) {
        return;
    }
    stop();
    give_back();
    lv_free(m_small);
    m_small = nullptr;
    m_whole = false;
    m_shown = 0;
    lv_obj_set_style_bg_opa(m_root, LV_OPA_COVER, 0);
    lv_obj_invalidate(m_root);
}

void again_soon(lv_timer_t *&timer, void (*fn)())
{
    if (timer != nullptr) {
        lv_timer_reset(timer);
        lv_timer_resume(timer);
        return;
    }
    timer = lv_timer_create(
        [](lv_timer_t *t) {
            lv_timer_pause(t);
            reinterpret_cast<void (*)()>(lv_timer_get_user_data(t))();
        },
        RETRY_MS, reinterpret_cast<void *>(fn));
}

}  // namespace ui::kit
