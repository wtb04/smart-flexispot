#include "kit.h"

#include "kit_fonts.h"
#include "kit_pixels.h"
#include "lvgl_private.h"  // areas, and what a cover check asks; the version is pinned in dependencies.lock

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(ESP_PLATFORM)
#include "board.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace ui::kit {
namespace {

PopGlass       s_how    = PopGlass::Frozen;
PopMotion      s_motion = PopMotion::Grow;
char           s_freeze_report[96] = "";
char           s_up_report[96]     = "";
char           s_ahead_report[96]  = "";
char           s_card_report[96]   = "";
float          s_picture_ms = 0, s_shadow_ms = 0;
lv_draw_buf_t *s_frozen = nullptr;

// One picture of the screen, blurred: brought down to a quarter, smeared with
// box passes there, laid under the scrim's tint and stretched back. Done once,
// as the popover opens. On the panel the PPA does both stretches; the CPU only
// blurs the quarter-size picture, a sixteenth of the pixels.
constexpr int FW = size::screen_w, FH = size::screen_h, F = 8, SW = FW / F, SH = FH / F;
lv_draw_buf_t *s_shot = nullptr, *s_half = nullptr, *s_quarter = nullptr, *s_small = nullptr;
// the blurred picture in 24-bit, B, G, R as the PPA takes it: kept at 16-bit,
// neighbours in a soft area come out the same and stretch into flat plateaus
lv_draw_buf_t *s_blurred = nullptr;
lv_draw_buf_t *s_strip   = nullptr;  // the 24-bit strip the PPA stretches into, made on LVGL's task
std::atomic<bool> s_open{false};      // a popover is up: its picture is not to be touched

#if defined(ESP_PLATFORM)
ppa_client_handle_t s_ppa = nullptr;

bool stretch(const lv_draw_buf_t *from, lv_draw_buf_t *to, float scale)
{
    if (s_ppa == nullptr) {
        ppa_client_config_t client{};
        client.oper_type = PPA_OPERATION_SRM;
        if (ppa_register_client(&client, &s_ppa) != ESP_OK) {
            return false;
        }
    }
    ppa_srm_oper_config_t op{};
    op.in.buffer         = from->data;
    op.in.pic_w          = from->header.w;
    op.in.pic_h          = from->header.h;
    op.in.block_w        = from->header.w;
    op.in.block_h        = from->header.h;
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer        = to->data;
    op.out.buffer_size   = to->data_size;
    op.out.pic_w         = to->header.w;
    op.out.pic_h         = to->header.h;
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x           = scale;
    op.scale_y           = scale;
    op.mode              = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
}

// Stretched up by the PPA into 24-bit colour a strip at a time, then dithered
// down, so the soft picture does not break into the blobs a stretch straight
// into RGB565 leaves. A row of the small picture either side of each strip
// keeps the bilinear stretch seamless across them.
bool stretch_up_dithered(lv_draw_buf_t *small, lv_draw_buf_t *to, int scale)
{
    constexpr int ROWS = 10;
    const int sw = static_cast<int>(small->header.w), sh = static_cast<int>(small->header.h);
    const int ow = static_cast<int>(to->header.w);
    lv_draw_buf_t *strip = s_strip;
    if (strip == nullptr) {
        return false;
    }
    esp_cache_msync(small->data, small->data_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    std::uint64_t in_ppa = 0, in_sync = 0, in_dither = 0;
    for (int r0 = 0; r0 < sh; r0 += ROWS) {
        const std::uint64_t a = perf::now_us();
        const int from = std::max(0, r0 - 1), last = std::min(sh - 1, r0 + ROWS), rows = last - from + 1;
        ppa_srm_oper_config_t op{};
        op.in.buffer         = small->data;
        op.in.pic_w          = sw;
        op.in.pic_h          = sh;
        op.in.block_w        = sw;
        op.in.block_h        = rows;
        op.in.block_offset_y = from;
        op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB888;
        op.out.buffer        = strip->data;
        op.out.buffer_size   = strip->data_size;
        op.out.pic_w         = strip->header.w;
        op.out.pic_h         = strip->header.h;
        op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB888;
        op.rotation_angle    = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x           = static_cast<float>(scale);
        op.scale_y           = static_cast<float>(scale);
        op.mode              = PPA_TRANS_MODE_BLOCKING;
        if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK) {
            return false;
        }
        const std::uint64_t b = perf::now_us();
        esp_cache_msync(strip->data, strip->data_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        const std::uint64_t c = perf::now_us();
        static const pixels::Ditherer dither;  // 12 KB of tables, in internal RAM
        const int skip = (r0 - from) * scale, keep = std::min(ROWS, sh - r0) * scale;
        for (int y = 0; y < keep; ++y) {
            const int oy = r0 * scale + y;
            dither.row(strip->data + (skip + y) * strip->header.stride,
                       reinterpret_cast<std::uint16_t *>(to->data + oy * to->header.stride), ow, oy);
        }
        in_ppa += b - a;
        in_sync += c - b;
        in_dither += perf::now_us() - c;
    }
    std::snprintf(s_up_report, sizeof s_up_report, "  stretching up: PPA %.1f ms, cache %.1f, dither %.1f\n",
                  static_cast<double>(in_ppa) / 1000.0, static_cast<double>(in_sync) / 1000.0,
                  static_cast<double>(in_dither) / 1000.0);
    return true;
}
#else
// The simulator's stand-in for the PPA: an average down, a bilinear stretch up.
bool stretch(const lv_draw_buf_t *from, lv_draw_buf_t *to, float scale)
{
    const int ow = static_cast<int>(to->header.w), oh = static_cast<int>(to->header.h);
    const int iw = static_cast<int>(from->header.w), ih = static_cast<int>(from->header.h);
    const auto px = [&](int x, int y) {
        return reinterpret_cast<const std::uint16_t *>(from->data + std::min(y, ih - 1) * from->header.stride)[std::min(x, iw - 1)];
    };
    if (scale < 1.0f) {
        const int k = static_cast<int>(1.0f / scale + 0.5f);
        for (int y = 0; y < oh; ++y) {
            auto *out = reinterpret_cast<std::uint16_t *>(to->data + y * to->header.stride);
            for (int x = 0; x < ow; ++x) {
                int r = 0, g = 0, b = 0;
                for (int dy = 0; dy < k; ++dy) {
                    for (int dx = 0; dx < k; ++dx) {
                        const std::uint16_t c = px(x * k + dx, y * k + dy);
                        r += c >> 11;
                        g += (c >> 5) & 0x3f;
                        b += c & 0x1f;
                    }
                }
                out[x] = static_cast<std::uint16_t>(((r / (k * k)) << 11) | ((g / (k * k)) << 5) | (b / (k * k)));
            }
        }
        return true;
    }
    for (int y = 0; y < oh; ++y) {
        auto *out = reinterpret_cast<std::uint16_t *>(to->data + y * to->header.stride);
        const float fy = std::max(0.0f, (y + 0.5f) / scale - 0.5f);
        const int   y0 = static_cast<int>(fy);
        const int   ty = static_cast<int>((fy - y0) * 256);
        for (int x = 0; x < ow; ++x) {
            const float fx = std::max(0.0f, (x + 0.5f) / scale - 0.5f);
            const int   x0 = static_cast<int>(fx);
            const int   tx = static_cast<int>((fx - x0) * 256);
            const std::uint16_t a = px(x0, y0), b = px(x0 + 1, y0), c = px(x0, y0 + 1), d = px(x0 + 1, y0 + 1);
            const auto mix = [&](int shift, int mask) {
                const int top = ((a >> shift) & mask) * (256 - tx) + ((b >> shift) & mask) * tx;
                const int bot = ((c >> shift) & mask) * (256 - tx) + ((d >> shift) & mask) * tx;
                return ((top * (256 - ty) + bot * ty) >> 16) & mask;
            };
            const auto eight = [&](int shift, int mask) {  // the mix in 8-bit, its fraction kept for the dither
                const int top = ((a >> shift) & mask) * (256 - tx) + ((b >> shift) & mask) * tx;
                const int bot = ((c >> shift) & mask) * (256 - tx) + ((d >> shift) & mask) * tx;
                return static_cast<int>((static_cast<std::int64_t>(top * (256 - ty) + bot * ty) * 255 / mask) >> 16);
            };
            out[x] = scale > 1.0f ? pixels::dither(eight(11, 0x1f), eight(5, 0x3f), eight(0, 0x1f), x, y)
                                  : static_cast<std::uint16_t>((mix(11, 0x1f) << 11) | (mix(5, 0x3f) << 5) | mix(0, 0x1f));
        }
    }
    return true;
}
#endif

#if !defined(ESP_PLATFORM)
bool stretch_up_dithered(lv_draw_buf_t *small, lv_draw_buf_t *to, int scale)
{
    const int sw = static_cast<int>(small->header.w), sh = static_cast<int>(small->header.h);
    const int ow = static_cast<int>(to->header.w), oh = static_cast<int>(to->header.h);
    const auto at = [&](int x, int y, int k) {
        return small->data[std::min(y, sh - 1) * small->header.stride + std::min(x, sw - 1) * 3 + k];
    };
    for (int y = 0; y < oh; ++y) {
        auto *out = reinterpret_cast<std::uint16_t *>(to->data + y * to->header.stride);
        const int fy = std::max(0, (2 * y + 1) * 128 / scale - 128), y0 = fy >> 8, ty = fy & 0xff;
        for (int x = 0; x < ow; ++x) {
            const int fx = std::max(0, (2 * x + 1) * 128 / scale - 128), x0 = fx >> 8, tx = fx & 0xff;
            int v[3];
            for (int k = 0; k < 3; ++k) {
                const int top = at(x0, y0, k) * (256 - tx) + at(x0 + 1, y0, k) * tx;
                const int bot = at(x0, y0 + 1, k) * (256 - tx) + at(x0 + 1, y0 + 1, k) * tx;
                v[k] = (top * (256 - ty) + bot * ty) >> 16;
            }
            out[x] = pixels::dither(v[2], v[1], v[0], x, y);
        }
    }
    return true;
}
#endif

// Memory for the blur, from outside LVGL's allocator, which belongs to its own
// task: on the panel the blur runs on the other core.
void *scratch(std::size_t bytes)
{
#if defined(ESP_PLATFORM)
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
    return std::malloc(bytes);
#endif
}

void unscratch(void *p)
{
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    std::free(p);
#endif
}

void blur_small()
{
    constexpr int R = 3, N = 2 * R + 1;
    using Plane = std::uint8_t[SH][SW];
    auto *ch  = static_cast<Plane *>(scratch(sizeof(Plane) * 3));  // PSRAM: too big for internal RAM
    auto *buf = static_cast<Plane *>(scratch(sizeof(Plane)));
    if (ch == nullptr || buf == nullptr) {
        unscratch(ch);
        unscratch(buf);
        return;
    }
    Plane &tmp = *buf;
    for (int y = 0; y < SH; ++y) {
        const auto *row = reinterpret_cast<const std::uint16_t *>(s_small->data + y * s_small->header.stride);
        for (int x = 0; x < SW; ++x) {
            ch[0][y][x] = static_cast<std::uint8_t>((row[x] >> 11) << 3);
            ch[1][y][x] = static_cast<std::uint8_t>(((row[x] >> 5) & 0x3f) << 2);
            ch[2][y][x] = static_cast<std::uint8_t>((row[x] & 0x1f) << 3);
        }
    }
    for (int plane = 0; plane < 3; ++plane) {
        Plane &c = ch[plane];
        for (int pass = 0; pass < 3; ++pass) {
            for (int y = 0; y < SH; ++y) {  // a running sum along each row, then each column
                int sum = c[y][0] * (R + 1);
                for (int x = 1; x <= R; ++x) {
                    sum += c[y][x];
                }
                for (int x = 0; x < SW; ++x) {
                    tmp[y][x] = static_cast<std::uint8_t>(sum / N);
                    sum += c[y][std::min(x + R + 1, SW - 1)] - c[y][std::max(x - R, 0)];
                }
            }
            // down the columns a row at a time, all columns at once, so memory is read in order
            static std::int32_t sums[SW];
            for (int x = 0; x < SW; ++x) {
                sums[x] = tmp[0][x] * (R + 1);
            }
            for (int y = 1; y <= R; ++y) {
                for (int x = 0; x < SW; ++x) {
                    sums[x] += tmp[y][x];
                }
            }
            for (int y = 0; y < SH; ++y) {
                const std::uint8_t *add = tmp[std::min(y + R + 1, SH - 1)], *drop = tmp[std::max(y - R, 0)];
                for (int x = 0; x < SW; ++x) {
                    c[y][x] = static_cast<std::uint8_t>(sums[x] / N);
                    sums[x] += add[x] - drop[x];
                }
            }
        }
    }
    const std::uint32_t tint = colours().scrim;
    const int a = colours().scrim_opa, t[3] = {static_cast<int>((tint >> 16) & 0xff), static_cast<int>((tint >> 8) & 0xff),
                                               static_cast<int>(tint & 0xff)};
    for (int y = 0; y < SH; ++y) {
        std::uint8_t *row = s_blurred->data + y * s_blurred->header.stride;
        for (int x = 0; x < SW; ++x) {
            for (int k = 0; k < 3; ++k) {
                row[x * 3 + 2 - k] = static_cast<std::uint8_t>((ch[k][y][x] * (255 - a) + t[k] * a) / 255);
            }
        }
    }
    unscratch(ch);
    unscratch(buf);
}

// The card's shadow painted into the picture once, where the card comes to
// rest, so the card draws none while it moves: a soft shadow of a moving
// shape is recomputed every frame, and is most of what a frame costs. Worked
// out here and dithered, as LVGL's own breaks into rings on RGB565.
constexpr std::int32_t SHADOW_W = 80, SHADOW_Y = 28, SHADOW_SPREAD = -8;  // big glass's own

// A rounded rectangle's distance, below zero inside; the square root only in
// its corners, as along its sides a step across is a step nearer.
float distance(float x, float y, float cx, float cy, float hx, float hy, float r)
{
    const float qx = std::fabs(x - cx) - hx, qy = std::fabs(y - cy) - hy;
    if (qx > 0.0f && qy > 0.0f) {
        return std::sqrt(qx * qx + qy * qy) - r;
    }
    return std::max(qx, qy) - r;
}

void bake_shadow(const lv_area_t &at, const lv_area_t &reach)
{
    // the shadow's edge, half dark, is the card's own pulled in by the spread and pushed down
    const float r  = static_cast<float>(std::max<std::int32_t>(0, radius::pop + SHADOW_SPREAD));
    const float cx = (at.x1 + at.x2 + 1) / 2.0f, cy = (at.y1 + at.y2 + 1) / 2.0f + SHADOW_Y;
    const float hx = lv_area_get_width(&at) / 2.0f + SHADOW_SPREAD - r;
    const float hy = lv_area_get_height(&at) / 2.0f + SHADOW_SPREAD - r;
    const std::uint32_t c = colours().shadow;
    const int sr = static_cast<int>((c >> 16) & 0xff), sg = static_cast<int>((c >> 8) & 0xff), sb = static_cast<int>(c & 0xff);
    // how dark at each quarter pixel from the edge, out to a little past
    // LVGL's half width, for the longer tail its blur has. LVGL rounds each
    // blend down, which on the dark ground makes its shadows darker than they
    // say by half again: as dark as those the look was set by.
    constexpr int SOFT = SHADOW_W * 6 / 10, STEPS = SOFT * 8;
    static std::uint16_t dark[STEPS + 1];
    const float opa = colours().glass_shadow_opa * (look() == Look::Dark ? 1.45f : 1.0f);
    for (int i = 0; i <= STEPS; ++i) {
        const float t = static_cast<float>(i) / STEPS;
        dark[i] = static_cast<std::uint16_t>(std::min(256.0f, opa * (1.0f - t * t * (3.0f - 2.0f * t)) * 256.0f / 255.0f));
    }
    const float card_r = radius::pop, card_cy = cy - SHADOW_Y;
    const float card_hx = lv_area_get_width(&at) / 2.0f - card_r, card_hy = lv_area_get_height(&at) / 2.0f - card_r;
    for (std::int32_t y = reach.y1; y <= reach.y2; ++y) {
        auto *row = reinterpret_cast<std::uint16_t *>(s_frozen->data + y * s_frozen->header.stride);
        const bool card_rows = y >= at.y1 && y <= at.y2, straight_rows = y >= at.y1 + radius::pop && y <= at.y2 - radius::pop;
        for (std::int32_t x = reach.x1; x <= reach.x2; ++x) {
            if (card_rows && x >= at.x1 && x <= at.x2) {
                if (straight_rows || (x >= at.x1 + radius::pop && x <= at.x2 - radius::pop)) {
                    x = straight_rows ? at.x2 : at.x2 - radius::pop;  // under the card, which casts nothing through itself
                    continue;
                }
                if (distance(x + 0.5f, y + 0.5f, cx, card_cy, card_hx, card_hy, card_r) < 0.0f) {
                    continue;
                }
            }
            const float d = distance(x + 0.5f, y + 0.5f, cx, cy, hx, hy, r);
            const int i = static_cast<int>((d + SOFT) * 4.0f);
            const int a = i <= 0 ? dark[0] : i >= STEPS ? 0 : dark[i];
            if (a == 0) {
                if (x > cx) {
                    break;  // past its right side, where it only gets lighter
                }
                continue;
            }
            const std::uint16_t px = row[x];
            const int pr = (px >> 11) << 3, pg = ((px >> 5) & 0x3f) << 2, pb = (px & 0x1f) << 3;
            row[x] = pixels::dither(pr + (sr - pr) * a / 256, pg + (sg - pg) * a / 256, pb + (sb - pb) * a / 256, x, y);
        }
    }
}

bool buffers()
{
    if (s_frozen == nullptr) {
        s_half    = lv_draw_buf_create(FW / 2, FH / 2, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        s_quarter = lv_draw_buf_create(FW / 4, FH / 4, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        s_small   = lv_draw_buf_create(SW, SH, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        s_blurred = lv_draw_buf_create(SW, SH, LV_COLOR_FORMAT_RGB888, LV_STRIDE_AUTO);
        s_frozen  = lv_draw_buf_create(FW, FH, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
#if defined(ESP_PLATFORM)
        s_strip = lv_draw_buf_create(FW, 12 * F, LV_COLOR_FORMAT_RGB888, LV_STRIDE_AUTO);
#endif
    }
    return s_half != nullptr && s_quarter != nullptr && s_small != nullptr && s_blurred != nullptr && s_frozen != nullptr;
}

// From the half-size picture on: no LVGL in it, so the panel's other core can do it.
void finish_picture(std::uint64_t t0, std::uint64_t t1)
{
    // halvings rather than one eighth: a bilinear halving is an average of each
    // two by two, where one big step would sample a few pixels of each block
    // and alias into blobs
    stretch(s_half, s_quarter, 0.5f);
    stretch(s_quarter, s_small, 0.5f);
#if defined(ESP_PLATFORM)
    esp_cache_msync(s_small->data, s_small->data_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
#endif
    const std::uint64_t t2 = perf::now_us();
    blur_small();
    const std::uint64_t t3 = perf::now_us();
    stretch_up_dithered(s_blurred, s_frozen, F);
    const std::uint64_t t4 = perf::now_us();
    std::snprintf(s_freeze_report, sizeof s_freeze_report,
                  "  freezing for a popover: picture %.1f ms, down %.1f, blur %.1f, up %.1f\n",
                  static_cast<double>(t1 - t0) / 1000.0, static_cast<double>(t2 - t1) / 1000.0,
                  static_cast<double>(t3 - t2) / 1000.0, static_cast<double>(t4 - t3) / 1000.0);
}

#if defined(ESP_PLATFORM)
// The panel's backdrop, made ahead on the core LVGL does not use: when a
// finger lands on something that opens a popover, and whenever the screen has
// been still for a while after changing. By the time the popover opens, its
// picture is usually there already.
TaskHandle_t               s_worker = nullptr;
std::atomic<bool>          s_working{false};
std::atomic<std::uint32_t> s_made_at_gen{0};
std::atomic<std::uint64_t> s_made_from_us{0}, s_pressed_us{0};
constexpr std::uint64_t    STILL_US = 250000, EVERY_US = 2000000;

bool picture_from_shown()
{
    const std::uint64_t t0 = perf::now_us();
    const std::uint32_t gen = perf::generation();
    if (board::shrink_shown(s_half->data, s_half->data_size, FW / 2, FH / 2) != ESP_OK) {
        return false;
    }
    finish_picture(t0, perf::now_us());
    s_made_at_gen.store(gen, std::memory_order_relaxed);
    s_made_from_us.store(t0, std::memory_order_relaxed);
    return true;
}

void work(void *)
{
    for (;;) {
        const bool kicked = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250)) > 0;
        const bool stale  = perf::generation() != s_made_at_gen.load(std::memory_order_relaxed);
        // a pause in the drawing, and not more often than every couple of seconds:
        // the screen rarely stays still for long, a clock or a plane moves
        const bool still  = perf::now_us() - perf::last_frame_us() > STILL_US;
        const bool due    = perf::now_us() - s_made_from_us.load(std::memory_order_relaxed) > EVERY_US;
        if (s_open.load(std::memory_order_acquire) || !(kicked || (stale && still && due))) {
            continue;
        }
        s_working.store(true, std::memory_order_release);
        picture_from_shown();
        s_working.store(false, std::memory_order_release);
    }
}

void start_worker()
{
    if (s_worker == nullptr && buffers()) {
        xTaskCreatePinnedToCore(work, "backdrop", 4096, nullptr, 2, &s_worker, 0);
    }
}
#endif

// On LVGL's task, as a popover opens: the picture made ahead if it is still
// the screen, else one made now.
void freeze()
{
    if (!buffers()) {
        return;
    }
#if defined(ESP_PLATFORM)
    start_worker();
    const std::uint64_t waited = perf::now_us();
    while (s_working.load(std::memory_order_acquire) && perf::now_us() - waited < 400000) {
        vTaskDelay(1);
    }
    const bool same    = perf::generation() == s_made_at_gen.load(std::memory_order_relaxed);
    const bool pressed = s_made_from_us.load(std::memory_order_relaxed) >= s_pressed_us.load(std::memory_order_relaxed) &&
                         perf::now_us() - s_pressed_us.load(std::memory_order_relaxed) < 1000000;
    if (same || pressed) {
        std::snprintf(s_ahead_report, sizeof s_ahead_report, "  last popover: picture made ahead, waited %.1f ms\n",
                      static_cast<double>(perf::now_us() - waited) / 1000.0);
        return;
    }
    if (picture_from_shown()) {
        std::snprintf(s_ahead_report, sizeof s_ahead_report, "  last popover: picture made as it opened, %.1f ms\n",
                      static_cast<double>(perf::now_us() - waited) / 1000.0);
        return;
    }
#endif
    // no frame on show to shrink, as in the simulator: draw the screen again
    const std::uint64_t t0 = perf::now_us();
    if (s_shot == nullptr) {
        s_shot = lv_draw_buf_create(FW, FH, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    }
    if (s_shot == nullptr || lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, s_shot) != LV_RESULT_OK) {
        return;
    }
    stretch(s_shot, s_half, 0.5f);
    finish_picture(t0, perf::now_us());
}

constexpr std::int32_t MARGIN = space::l, GAP = space::m, PROGRESS = 1024;
constexpr std::int32_t RISE  = 16;  // how far a rising card comes up
constexpr int          WAVES = 4;   // the frames the live screen comes back in as a popover goes

struct Pop {
    lv_obj_t *scrim    = nullptr;
    lv_obj_t *backdrop = nullptr;  // the frozen picture, outside what has been given back to the live screen
    lv_obj_t *card     = nullptr;
    lv_obj_t *body     = nullptr;
    lv_obj_t *source   = nullptr;
    lv_area_t from{}, to{};
    std::int32_t p        = 0;
    bool      closing     = false;
    bool      waiting     = false;  // to start once the first frame, the backdrop's, is out
    bool      pictured    = false;  // the card is drawn from its picture
    bool      shadowed    = false;  // its shadow is painted into the backdrop
    lv_opa_t  glass_opa   = 0;
    lv_draw_buf_t *picture = nullptr;  // the card at its place, over its backdrop and without its edge
    lv_area_t      picture_at{};
    lv_draw_buf_t *under = nullptr;    // the backdrop where the shadow went, to take it out again
    lv_area_t      under_at{};
    bool          rows   = true;        // the live screen comes back in bands of rows, or of columns
    std::int32_t  centre = 0, reach = 0, lo = 1, hi = 0;  // given back so far: lo to hi
    int           wave   = 0;
    std::uint32_t wave_frame = 0, wave_ms = 0;
    lv_timer_t   *stepper = nullptr;
} s_pop;

std::uint32_t s_frames = 0;
lv_obj_t     *s_notice = nullptr;

std::int32_t lerp(std::int32_t a, std::int32_t b, std::int32_t p) { return a + (b - a) * p / PROGRESS; }

constexpr lv_area_t SCREEN{0, 0, size::screen_w - 1, size::screen_h - 1};

lv_area_t on_screen(lv_area_t a)
{
    lv_area_t out{};
    if (!lv_area_intersect(&out, &a, &SCREEN)) {
        return {0, 0, -1, -1};
    }
    return out;
}

lv_area_t span(const lv_area_t &a, const lv_area_t &b)
{
    return {std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2), std::max(a.y2, b.y2)};
}

// --- the backdrop ---------------------------------------------------------

// The part of the screen the frozen picture still covers, along the bands:
// everything but lo to hi.
bool still_frozen(std::int32_t a, std::int32_t b) { return s_pop.lo > s_pop.hi || b < s_pop.lo || a > s_pop.hi; }

// The frozen picture as an object of its own rather than the overlay's
// background, so it can be taken away a band at a time. Opaque wherever it
// still is: LVGL starts drawing there and skips everything under it.
void on_backdrop(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_COVER_CHECK) {
        auto *info = static_cast<lv_cover_check_info_t *>(lv_event_get_param(e));
        if (info->res != LV_COVER_RES_MASKED) {
            const lv_area_t *a = info->area;
            info->res = still_frozen(s_pop.rows ? a->y1 : a->x1, s_pop.rows ? a->y2 : a->x2) ? LV_COVER_RES_COVER
                                                                                        : LV_COVER_RES_NOT_COVER;
        }
        return;
    }
    if (code != LV_EVENT_DRAW_MAIN || s_frozen == nullptr) {
        return;
    }
    lv_layer_t *paint = lv_event_get_layer(e);
    const lv_area_t clip = paint->_clip_area;
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = s_frozen;
    // the whole picture each time, cut to what is left: a plain copy the PPA takes
    lv_area_t parts[2] = {SCREEN, SCREEN};
    const int n = s_pop.lo > s_pop.hi ? 1 : 2;
    if (n == 2) {
        (s_pop.rows ? parts[0].y2 : parts[0].x2) = s_pop.lo - 1;
        (s_pop.rows ? parts[1].y1 : parts[1].x1) = s_pop.hi + 1;
    }
    for (int i = 0; i < n; ++i) {
        lv_area_t part;
        if (lv_area_intersect(&part, &parts[i], &clip)) {
            paint->_clip_area = part;
            lv_draw_image(paint, &dsc, &SCREEN);
        }
    }
    paint->_clip_area = clip;
}

lv_area_t shadow_area(const lv_area_t &at)
{
    constexpr std::int32_t REACH = SHADOW_W / 2 + 2;
    return on_screen({at.x1 - REACH, at.y1 + SHADOW_Y - REACH, at.x2 + REACH, at.y2 + SHADOW_Y + REACH});
}

void copy_rows(lv_draw_buf_t *to, const lv_area_t &to_at, const lv_draw_buf_t *from, const lv_area_t &from_at,
               const lv_area_t &a)
{
    const std::int32_t w = lv_area_get_width(&a);
    for (std::int32_t y = a.y1; y <= a.y2; ++y) {
        std::memcpy(to->data + (y - to_at.y1) * to->header.stride + (a.x1 - to_at.x1) * 2,
                    from->data + (y - from_at.y1) * from->header.stride + (a.x1 - from_at.x1) * 2,
                    static_cast<std::size_t>(w) * 2);
    }
}

// The card's shadow into the backdrop, keeping what it covers so it can go
// with the card.
void shadow_on(const lv_area_t &at)
{
    s_pop.under_at = shadow_area(at);
    s_pop.under    = lv_draw_buf_create(lv_area_get_width(&s_pop.under_at), lv_area_get_height(&s_pop.under_at),
                                        LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (s_pop.under == nullptr) {
        return;
    }
    copy_rows(s_pop.under, s_pop.under_at, s_frozen, SCREEN, s_pop.under_at);
    const std::uint64_t t0 = perf::now_us();
    bake_shadow(at, s_pop.under_at);
    s_shadow_ms = static_cast<float>(perf::now_us() - t0) / 1000.0f;
    s_pop.shadowed = true;
#if defined(ESP_PLATFORM)
    s_made_at_gen.store(0, std::memory_order_relaxed);  // a shadow painted in: never this picture again
    s_made_from_us.store(0, std::memory_order_relaxed);
#endif
    if (s_pop.backdrop != nullptr) {
        lv_obj_invalidate_area(s_pop.backdrop, &s_pop.under_at);
    }
}

void shadow_off()
{
    if (s_pop.under == nullptr) {
        return;
    }
    if (s_pop.shadowed) {
        copy_rows(s_frozen, SCREEN, s_pop.under, s_pop.under_at, s_pop.under_at);
        if (s_pop.backdrop != nullptr) {
            lv_obj_invalidate_area(s_pop.backdrop, &s_pop.under_at);
        }
    }
    lv_draw_buf_destroy(s_pop.under);
    s_pop.under    = nullptr;
    s_pop.shadowed = false;
}

// --- the card's picture ---------------------------------------------------

void drop_picture()
{
    if (s_pop.picture != nullptr) {
        lv_image_cache_drop(s_pop.picture);
        lv_draw_buf_destroy(s_pop.picture);
        s_pop.picture = nullptr;
    }
}

// The card as it looks at rest, drawn once over the backdrop it rests on: a
// frame of it growing or rising is then a copy of part of this, rather than
// its glass, its contents and their shadows all drawn again. `at` may reach
// past the card, where the glass goes on plain, for an outline that comes
// from outside it.
bool paint_picture(lv_area_t at)
{
    at = on_screen(at);
    const std::int32_t w = lv_area_get_width(&at), h = lv_area_get_height(&at);
    if (s_frozen == nullptr || w <= 0 || h <= 0) {
        return false;
    }
    if (s_pop.picture == nullptr || s_pop.picture->header.w != static_cast<std::uint32_t>(w) ||
        s_pop.picture->header.h != static_cast<std::uint32_t>(h)) {
        drop_picture();
        s_pop.picture = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
        if (s_pop.picture == nullptr) {
            return false;
        }
    }
    const std::uint64_t t0 = perf::now_us();
    copy_rows(s_pop.picture, at, s_frozen, SCREEN, at);

    lv_obj_set_pos(s_pop.card, s_pop.to.x1, s_pop.to.y1);
    lv_obj_set_size(s_pop.card, lv_area_get_width(&s_pop.to), lv_area_get_height(&s_pop.to));
    lv_obj_set_pos(s_pop.body, 0, 0);
    lv_obj_remove_flag(s_pop.body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_pop.card);

    lv_obj_t *canvas = lv_canvas_create(layer(Layer::Overlay));
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(canvas, s_pop.picture);
    lv_layer_t paint;
    lv_canvas_init_layer(canvas, &paint);
    paint.buf_area = paint._clip_area = paint.phy_clip_area = at;
    lv_draw_rect_dsc_t glass;
    lv_draw_rect_dsc_init(&glass);
    glass.bg_color      = lv_obj_get_style_bg_color(s_pop.card, LV_PART_MAIN);
    glass.bg_opa        = s_pop.glass_opa;
    glass.radius        = 0;
    glass.border_width  = 0;
    glass.outline_width = 0;
    glass.shadow_width  = 0;
    lv_draw_rect(&paint, &glass, &at);
    lv_obj_redraw(&paint, s_pop.body);
    lv_canvas_finish_layer(canvas, &paint);
    lv_obj_delete(canvas);
    lv_image_cache_drop(s_pop.picture);  // drawn into again: nothing kept of what it was
    s_pop.picture_at = at;
    s_picture_ms = static_cast<float>(perf::now_us() - t0) / 1000.0f;
    std::snprintf(s_card_report, sizeof s_card_report, "  last popover: its picture %.1f ms, its shadow %.1f ms\n",
                  static_cast<double>(s_picture_ms), static_cast<double>(s_shadow_ms));
    return true;
}

void show_picture(bool on)
{
    s_pop.pictured = on && s_pop.picture != nullptr;
    if (s_pop.pictured) {
        lv_obj_add_flag(s_pop.body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(s_pop.card, LV_OPA_TRANSP, 0);
    } else {
        lv_obj_remove_flag(s_pop.body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_local_style_prop(s_pop.card, LV_STYLE_BG_OPA, 0);
    }
}

// Under the card's own edge, which it still draws: the picture, inside the
// outline as it is now.
void draw_picture(lv_event_t *e)
{
    if (!s_pop.pictured) {
        return;
    }
    lv_layer_t *paint = lv_event_get_layer(e);
    lv_area_t card;
    lv_obj_get_coords(s_pop.card, &card);
    lv_area_t at = s_pop.picture_at;
    if (s_motion == PopMotion::Rise && !s_pop.closing) {
        lv_area_move(&at, card.x1 - s_pop.to.x1, card.y1 - s_pop.to.y1);  // what is on it comes up with it
    }
    const std::int32_t r = std::min({radius::pop, lv_area_get_width(&card) / 2, lv_area_get_height(&card) / 2});
    const lv_area_t clip = paint->_clip_area;
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = s_pop.picture;
    // the rows through the rounded corners masked, the rest a plain copy the PPA takes
    const lv_area_t rows[3] = {{card.x1, card.y1, card.x2, card.y1 + r - 1},
                               {card.x1, card.y1 + r, card.x2, card.y2 - r},
                               {card.x1, card.y2 - r + 1, card.x2, card.y2}};
    for (int i = 0; i < 3; ++i) {
        lv_area_t part;
        if (!lv_area_intersect(&part, &rows[i], &clip) || !lv_area_intersect(&part, &part, &at)) {
            continue;
        }
        paint->_clip_area = part;
        dsc.clip_radius   = i == 1 ? 0 : r;
        dsc.image_area    = i == 1 ? at : card;  // the corners' mask is the outline's
        lv_draw_image(paint, &dsc, &at);
    }
    paint->_clip_area = clip;
}

// --- motion ---------------------------------------------------------------

void run(std::int32_t from, std::int32_t to, std::uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done);
void back_in_source(lv_anim_t *);

// Grow: the outline opens out of the source while what is on the card stays
// still on the screen, so it reads as opening out of what was tapped. Rise:
// the card at its place from the start, coming up a little, so the area that
// changes is the card's own and never grows. Either goes back into the source.
// No fades: an object faded with what is on it is drawn off-screen first,
// every frame, which costs the panel more than all the rest.
void place(std::int32_t p)
{
    if (s_pop.card == nullptr) {
        return;
    }
    // the spring's last percent and a half is a pixel or two over a quarter
    // of a second, each a frame of the whole card: it ends where it would
    // have, rather than creeping there
    if (s_pop.closing ? p <= PROGRESS * 15 / 1000 : p >= PROGRESS * 985 / 1000) {
        p = s_pop.closing ? 0 : PROGRESS;
    }
    s_pop.p = p;
    if (s_motion == PopMotion::Grow || s_pop.closing) {
        const std::int32_t x = lerp(s_pop.from.x1, s_pop.to.x1, p), y = lerp(s_pop.from.y1, s_pop.to.y1, p);
        const std::int32_t w = std::max<std::int32_t>(lerp(lv_area_get_width(&s_pop.from), lv_area_get_width(&s_pop.to), p), 8);
        const std::int32_t h = std::max<std::int32_t>(lerp(lv_area_get_height(&s_pop.from), lv_area_get_height(&s_pop.to), p), 8);
        lv_obj_set_pos(s_pop.card, x, y);
        lv_obj_set_size(s_pop.card, w, h);
        lv_obj_set_pos(s_pop.body, s_pop.to.x1 - x, s_pop.to.y1 - y);
    } else {
        lv_obj_set_pos(s_pop.card, s_pop.to.x1, s_pop.to.y1 + RISE * (PROGRESS - p) / PROGRESS);
        lv_obj_set_size(s_pop.card, lv_area_get_width(&s_pop.to), lv_area_get_height(&s_pop.to));
        lv_obj_set_pos(s_pop.body, 0, 0);
    }
    // the shadow lands with the card, in the last few percent, rather than after it has come to rest
    if (!s_pop.closing && !s_pop.shadowed && s_pop.backdrop != nullptr && p >= PROGRESS * 97 / 100) {
        shadow_on(s_pop.to);
    }
    if (s_pop.closing && p == 0) {
        back_in_source(nullptr);
    }
}

void landed(lv_anim_t *)
{
    show_picture(false);  // from here on what is on it is live
    if (s_pop.card != nullptr) {
        lv_obj_invalidate(s_pop.card);
    }
}

void picture_card(void *)
{
    if (s_pop.card == nullptr || s_pop.closing || s_pop.pictured || s_pop.backdrop == nullptr) {
        return;
    }
    if (paint_picture(s_motion == PopMotion::Grow ? span(s_pop.from, s_pop.to) : s_pop.to)) {
        show_picture(true);
    }
    place(s_pop.p);
}

void finish_close()
{
    // all given back: the full-screen objects go without drawing the screen again
    lv_display_t *disp = lv_display_get_default();
    const bool quiet = s_pop.backdrop != nullptr && s_pop.wave >= WAVES && lv_display_is_invalidation_enabled(disp);
    if (quiet) {
        lv_display_enable_invalidation(disp, false);
    }
    s_open.store(false, std::memory_order_release);
    if (s_pop.source != nullptr) {
        lv_obj_remove_state(s_pop.source, LV_STATE_USER_1);
    }
    if (s_pop.stepper != nullptr) {
        lv_timer_delete(s_pop.stepper);
    }
    shadow_off();
    drop_picture();
    for (lv_obj_t *obj : {s_pop.scrim, s_pop.card, s_pop.backdrop}) {
        if (obj != nullptr) {
            lv_obj_delete(obj);
        }
    }
    s_pop = Pop{};
    if (quiet) {
        lv_display_enable_invalidation(disp, true);
    }
}

// The live screen back from where the card went, a band of the screen each
// frame, outward: drawing all of it again in one frame is the slowest frame
// the panel has, and this is the same work spread over a few.
void give_back()
{
    const std::int32_t length = s_pop.rows ? size::screen_h : size::screen_w;
    const std::int32_t r = s_pop.reach * ++s_pop.wave / WAVES;
    const std::int32_t lo = std::max<std::int32_t>(0, s_pop.centre - r), hi = std::min<std::int32_t>(length - 1, s_pop.centre + r);
    const auto band = [](std::int32_t a, std::int32_t b) {
        if (a > b) {
            return;
        }
        lv_area_t area = s_pop.rows ? lv_area_t{0, a, size::screen_w - 1, b} : lv_area_t{a, 0, b, size::screen_h - 1};
        lv_obj_invalidate_area(s_pop.backdrop, &area);
    };
    if (s_pop.lo > s_pop.hi) {
        band(lo, hi);
    } else {
        band(lo, s_pop.lo - 1);
        band(s_pop.hi + 1, hi);
    }
    s_pop.lo = lo;
    s_pop.hi = hi;
    if (s_pop.card != nullptr) {  // it is back in its source, which is in the first band
        lv_obj_delete(s_pop.card);
        s_pop.card = s_pop.body = nullptr;
    }
    s_pop.wave_frame = s_frames;
    s_pop.wave_ms    = lv_tick_get();
}

void back_in_source(lv_anim_t *)
{
    if (s_pop.wave > 0 || s_pop.scrim == nullptr) {
        return;  // already on its way back
    }
    if (s_pop.backdrop == nullptr) {
        finish_close();
        return;
    }
    const lv_area_t &src = s_pop.from;
    s_pop.centre = s_pop.rows ? (src.y1 + src.y2) / 2 : (src.x1 + src.x2) / 2;
    const std::int32_t length = s_pop.rows ? size::screen_h : size::screen_w;
    s_pop.reach = std::max(s_pop.centre, length - 1 - s_pop.centre) + 1;
    give_back();
    s_pop.stepper = lv_timer_create(
        [](lv_timer_t *) {
            // a band a frame, or on regardless if no frame comes
            if (s_frames == s_pop.wave_frame && lv_tick_elaps(s_pop.wave_ms) < 150) {
                return;
            }
            if (s_pop.wave >= WAVES) {
                finish_close();
            } else {
                give_back();
            }
        },
        1, nullptr);
}

void run(std::int32_t from, std::int32_t to, std::uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_delete(&s_pop, nullptr);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_pop);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_custom_exec_cb(&a, [](lv_anim_t *, int32_t v) { place(v); });
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    if (done != nullptr) {
        lv_anim_set_completed_cb(&a, done);
    }
    lv_anim_start(&a);
}

// The clock starts once the frame that brings the backdrop is out, so the
// first frame of the motion is its start and not a jump a slow frame long.
void on_frame(lv_event_t *)
{
    ++s_frames;
    if (s_pop.waiting) {
        s_pop.waiting = false;
        run(0, PROGRESS, motion::pop_ms, motion::spring, landed);
    }
}

}  // namespace

bool popover_open() { return s_pop.card != nullptr && !s_pop.closing; }

void prepare_popover()
{
#if defined(ESP_PLATFORM)
    start_worker();
    s_pressed_us.store(perf::now_us(), std::memory_order_relaxed);
    if (s_worker != nullptr) {
        xTaskNotifyGive(s_worker);
    }
#endif
}

void popover_source(lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, [](lv_event_t *) { prepare_popover(); }, LV_EVENT_PRESSED, nullptr);
}
void set_pop_glass(PopGlass how) { s_how = how; }
void set_pop_motion(PopMotion how) { s_motion = how; }
PopMotion pop_motion() { return s_motion; }
const lv_draw_buf_t *frozen_picture() { return s_frozen; }

const char *freeze_report()
{
    static char both[400];
    std::snprintf(both, sizeof both, "%s%s%s%s", s_freeze_report, s_up_report, s_ahead_report, s_card_report);
    return both;
}
PopGlass pop_glass() { return s_how; }

lv_obj_t *popover(lv_obj_t *source, std::int32_t w, std::int32_t h, From from, bool blur_scrim)
{
    close_popover(false);
    lv_obj_t *top = layer(Layer::Overlay);  // not LVGL's top layer: glass there has nothing under it to blur
    lv_area_t src;
    lv_obj_get_coords(source, &src);

    std::int32_t x, y;
    if (from == From::Below) {
        x = std::clamp<std::int32_t>((src.x1 + src.x2) / 2 - w / 2, MARGIN, size::screen_w - MARGIN - w);
        y = src.y2 + GAP;
        if (y + h > size::screen_h - MARGIN) {
            y = size::screen_h - MARGIN - h;
        }
    } else {
        x = src.x2 + GAP;
        y = std::clamp<std::int32_t>(src.y1, MARGIN, size::screen_h - MARGIN - h);
    }
    s_pop.to     = {x, y, x + w - 1, y + h - 1};
    s_pop.from   = src;
    s_pop.source = source;
    s_pop.rows   = from == From::Below;
    lv_obj_add_state(source, LV_STATE_USER_1);

    static bool watching = false;
    if (!watching) {
        watching = true;
        lv_display_add_event_cb(lv_obj_get_display(top), on_frame, LV_EVENT_REFR_READY, nullptr);
    }

    const bool frozen = s_how == PopGlass::Frozen;
    if (frozen) {
        freeze();
    }
    s_open.store(true, std::memory_order_release);
    if (frozen && s_frozen != nullptr) {
        s_pop.backdrop = lv_obj_create(top);
        lv_obj_remove_style_all(s_pop.backdrop);
        lv_obj_set_size(s_pop.backdrop, size::screen_w, size::screen_h);
        lv_obj_remove_flag(s_pop.backdrop, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_pop.backdrop, on_backdrop, LV_EVENT_COVER_CHECK, nullptr);
        lv_obj_add_event_cb(s_pop.backdrop, on_backdrop, LV_EVENT_DRAW_MAIN, nullptr);
    }
    s_pop.scrim = lv_obj_create(top);
    lv_obj_remove_style_all(s_pop.scrim);
    lv_obj_set_size(s_pop.scrim, size::screen_w, size::screen_h);
    if (s_pop.backdrop == nullptr) {
        lv_obj_set_style_bg_color(s_pop.scrim, lv_color_hex(colours().scrim), 0);
        lv_obj_set_style_bg_opa(s_pop.scrim, colours().scrim_opa, 0);
        if (blur_scrim) {
            lv_obj_set_style_blur_backdrop(s_pop.scrim, true, 0);
            lv_obj_set_style_blur_radius(s_pop.scrim, 6, 0);
        }
    }
    lv_obj_add_flag(s_pop.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pop.scrim, [](lv_event_t *) { close_popover(true); }, LV_EVENT_CLICKED, nullptr);

    s_pop.card = glass(top, Glass::Large);
    lv_obj_set_style_radius(s_pop.card, radius::pop, 0);
    s_pop.glass_opa = lv_obj_get_style_bg_opa(s_pop.card, LV_PART_MAIN);
    if (s_pop.backdrop == nullptr) {
        lv_obj_set_style_blur_backdrop(s_pop.card, true, 0);
        lv_obj_set_style_blur_radius(s_pop.card, 40, 0);
    } else {
        // the shadow is painted into the backdrop: a soft shadow of a moving
        // shape is drawn again every frame, and is most of what a frame costs
        lv_obj_set_style_shadow_width(s_pop.card, 0, 0);
        if (s_motion == PopMotion::Rise) {
            shadow_on(s_pop.to);
        }
        lv_obj_add_event_cb(s_pop.card, draw_picture, LV_EVENT_DRAW_MAIN_BEGIN, nullptr);
        lv_async_call(picture_card, nullptr);  // once whoever opened it has filled it
    }
    lv_obj_set_style_pad_all(s_pop.card, 0, 0);
    lv_obj_add_flag(s_pop.card, LV_OBJ_FLAG_CLICKABLE);

    s_pop.body = lv_obj_create(s_pop.card);
    lv_obj_remove_style_all(s_pop.body);
    lv_obj_set_size(s_pop.body, w, h);
    lv_obj_set_style_pad_all(s_pop.body, space::xxl, 0);
    lv_obj_remove_flag(s_pop.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pop.body, LV_OBJ_FLAG_IGNORE_LAYOUT);

    place(0);
    s_pop.waiting = true;
    return s_pop.body;
}

void close_popover(bool animate)
{
    if (s_pop.scrim == nullptr) {
        return;
    }
    if (!animate) {
        lv_anim_delete(&s_pop, nullptr);
        finish_close();
        return;
    }
    if (s_pop.closing) {
        return;
    }
    s_pop.closing = true;
    s_pop.waiting = false;
    if (s_pop.backdrop != nullptr) {
        shadow_off();  // the shadow goes with the card
        if (paint_picture(span(s_pop.from, s_pop.to))) {
            show_picture(true);
        }
        place(s_pop.p);
    }
    // quick, and into the source without passing it
    run(s_pop.p, 0, motion::close_ms, motion::settle, back_in_source);
}

// --- notices --------------------------------------------------------------

namespace motion {
void moving(lv_obj_t *obj, bool moving)
{
    if (!moving) {
        lv_obj_remove_local_style_prop(obj, LV_STYLE_SHADOW_WIDTH, 0);
        lv_obj_remove_local_style_prop(obj, LV_STYLE_SHADOW_OFFSET_Y, 0);
        return;
    }
    lv_obj_set_style_shadow_width(obj, std::min<std::int32_t>(lv_obj_get_style_shadow_width(obj, LV_PART_MAIN), 20), 0);
    lv_obj_set_style_shadow_offset_y(obj, 6, 0);
}
}  // namespace motion

namespace {
void drop(void *obj, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), y); }

void leave(lv_obj_t *slab)
{
    if (slab == nullptr || lv_obj_has_flag(slab, LV_OBJ_FLAG_USER_4)) {
        return;
    }
    lv_obj_add_flag(slab, LV_OBJ_FLAG_USER_4);
    lv_anim_delete(slab, nullptr);
    motion::moving(slab, true);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slab);
    lv_anim_set_values(&a, lv_obj_get_y(slab), -lv_obj_get_height(slab) - 40);
    lv_anim_set_exec_cb(&a, drop);
    lv_anim_set_duration(&a, motion::leave_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, [](lv_anim_t *an) {
        auto *gone = static_cast<lv_obj_t *>(an->var);
        if (gone == s_notice) {
            s_notice = nullptr;
        }
        lv_obj_delete_async(gone);
    });
    lv_anim_start(&a);
}
}  // namespace

lv_obj_t *notice(const char *source, const char *glyph, const char *title, const char *message,
                 std::uint32_t timeout_ms)
{
    if (s_notice != nullptr) {
        leave(s_notice);
    }
    constexpr std::int32_t W = 620, H = 150, Y = 100;
    lv_obj_t *slab = glass(layer(Layer::Alert), Glass::Large);
    s_notice       = slab;
    lv_obj_set_style_radius(slab, radius::pop, 0);
    lv_obj_set_size(slab, W, H);
    lv_obj_set_pos(slab, (size::screen_w - W) / 2, -H);
    lv_obj_set_style_pad_all(slab, space::xl, 0);
    lv_obj_set_flex_flow(slab, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(slab, space::l, 0);
    lv_obj_add_flag(slab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slab, [](lv_event_t *e) { leave(lv_event_get_target_obj(e)); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        slab,
        [](lv_event_t *e) {
            if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
                leave(lv_event_get_target_obj(e));  // flicked back up where it came from
            }
        },
        LV_EVENT_GESTURE, nullptr);

    lv_obj_t *mark = well(slab);
    lv_obj_set_size(mark, 76, 76);
    lv_obj_set_style_pad_all(mark, 0, 0);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);  // a tap on it is a tap on the notice
    lv_obj_center(icon(mark, glyph, 34, true));

    lv_obj_t *col = lv_obj_create(slab);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(text(head, Role::Label, source, Ink::Secondary), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(text(head, Role::Label, "09:41", Ink::Secondary), LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_pad_top(text(col, Role::H2, title), space::s, 0);
    text(col, Role::Small, message, Ink::Secondary);

    lv_obj_t *line = lv_obj_create(col);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), 3);
    lv_obj_set_style_bg_color(line, colour(Ink::Primary), 0);
    lv_obj_set_style_bg_opa(line, colours().ink3_opa, 0);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_set_style_margin_top(line, space::m, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);

    motion::moving(slab, true);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slab);
    lv_anim_set_values(&a, -H, Y);
    lv_anim_set_custom_exec_cb(&a, [](lv_anim_t *an, int32_t y) {
        auto *obj = static_cast<lv_obj_t *>(an->var);
        // the last pixels of the spring, where it has all but stopped, at
        // once: from there on it is still, its big shadow back
        if (y >= an->end_value - 3) {
            y = an->end_value;
            if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_3)) {
                lv_obj_add_flag(obj, LV_OBJ_FLAG_USER_3);
                motion::moving(obj, false);  // landed: its big shadow back
            }
        }
        lv_obj_set_y(obj, y);
    });
    lv_anim_set_duration(&a, motion::pop_ms);
    lv_anim_set_path_cb(&a, motion::spring);
    lv_anim_start(&a);

    if (timeout_ms > 0) {
        lv_anim_t t;
        lv_anim_init(&t);
        lv_anim_set_var(&t, line);
        lv_anim_set_values(&t, 100, 0);
        lv_anim_set_exec_cb(&t, [](void *obj, int32_t v) { lv_obj_set_width(static_cast<lv_obj_t *>(obj), lv_pct(v)); });
        lv_anim_set_duration(&t, timeout_ms);
        lv_anim_set_path_cb(&t, lv_anim_path_linear);
        lv_anim_set_completed_cb(&t, [](lv_anim_t *an) {
            lv_obj_t *slab_of = lv_obj_get_parent(lv_obj_get_parent(static_cast<lv_obj_t *>(an->var)));
            leave(slab_of);
        });
        lv_anim_start(&t);
    }
    return slab;
}

}  // namespace ui::kit
