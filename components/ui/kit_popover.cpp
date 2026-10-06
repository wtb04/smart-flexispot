#include "kit.h"

#include "kit_fonts.h"
#include "kit_pixels.h"

#include <algorithm>
#include <cstdio>
#include <atomic>
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
// shape is recomputed every frame, and is most of what a frame costs.
void bake_shadow(const lv_area_t &at)
{
    lv_obj_t *canvas = lv_canvas_create(layer(Layer::Overlay));
    lv_canvas_set_draw_buf(canvas, s_frozen);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_layer_t paint;
    lv_canvas_init_layer(canvas, &paint);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius        = radius::pop;
    dsc.bg_opa        = LV_OPA_TRANSP;
    dsc.shadow_width  = 70;
    dsc.shadow_offset_y = 24;
    dsc.shadow_spread = -10;
    dsc.shadow_color  = lv_color_hex(colours().shadow);
    dsc.shadow_opa    = colours().glass_shadow_opa;
    lv_draw_rect(&paint, &dsc, &at);
    lv_canvas_finish_layer(canvas, &paint);
    lv_obj_delete(canvas);
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

struct Pop {
    lv_obj_t *scrim  = nullptr;
    lv_obj_t *card   = nullptr;
    lv_obj_t *body   = nullptr;
    lv_obj_t *source = nullptr;
    lv_area_t from{}, to{};
    bool      closing = false;
} s_pop;

lv_obj_t *s_notice = nullptr;

// Setting a style marks the whole object for redrawing even when the value is
// the one it had, and the scrim is the whole screen: only ever set what changed.
void set_opa(lv_obj_t *obj, lv_opa_t opa)
{
    if (lv_obj_get_style_opa(obj, LV_PART_MAIN) != opa) {
        lv_obj_set_style_opa(obj, opa, 0);
    }
}

std::int32_t lerp(std::int32_t a, std::int32_t b, std::int32_t p) { return a + (b - a) * p / PROGRESS; }

// The card's outline grows from the source to its place while what is on it
// stays still on the screen, so it reads as opening out of what was tapped.
void rise(int32_t p);

void grow(void *, int32_t p)
{
    if (s_pop.card == nullptr) {
        return;
    }
    if (s_motion == PopMotion::Rise) {
        rise(p);
        return;
    }
    const std::int32_t x = lerp(s_pop.from.x1, s_pop.to.x1, p), y = lerp(s_pop.from.y1, s_pop.to.y1, p);
    const std::int32_t w = std::max<std::int32_t>(lerp(lv_area_get_width(&s_pop.from), lv_area_get_width(&s_pop.to), p), 8);
    const std::int32_t h = std::max<std::int32_t>(lerp(lv_area_get_height(&s_pop.from), lv_area_get_height(&s_pop.to), p), 8);
    lv_obj_set_pos(s_pop.card, x, y);
    lv_obj_set_size(s_pop.card, w, h);
    lv_obj_set_pos(s_pop.body, s_pop.to.x1 - x, s_pop.to.y1 - y);
    set_opa(s_pop.body, LV_OPA_COVER);  // the growing outline reveals it; a fade would draw it off-screen
    // the frozen scrim is a whole-screen picture: fade it in quickly, so only a few frames blend it all
    const lv_opa_t scrim = s_how == PopGlass::Frozen ? LV_OPA_COVER
                                                     : static_cast<lv_opa_t>(std::clamp<std::int32_t>(p * 255 / PROGRESS, 0, 255));
    set_opa(s_pop.scrim, scrim);
}

// The card at its place from the start, coming up a little and into view: the
// area that changes is the card's own and never grows.
void rise(int32_t p)
{
    // no fades: an object faded with what is on it is drawn off-screen first,
    // every frame, which costs the panel more than all the rest
    const std::int32_t lift = 14 * (PROGRESS - p) / PROGRESS;
    lv_obj_set_pos(s_pop.card, s_pop.to.x1, s_pop.to.y1 + lift);
    lv_obj_set_size(s_pop.card, lv_area_get_width(&s_pop.to), lv_area_get_height(&s_pop.to));
    lv_obj_set_pos(s_pop.body, 0, 0);
    set_opa(s_pop.body, LV_OPA_COVER);
    set_opa(s_pop.scrim, LV_OPA_COVER);
}

void finish_close(lv_anim_t *)
{
    s_open.store(false, std::memory_order_release);
    if (s_pop.source != nullptr) {
        lv_obj_remove_state(s_pop.source, LV_STATE_USER_1);
    }
    lv_obj_t *overlay = lv_obj_get_parent(s_pop.scrim);
    lv_obj_set_style_bg_image_src(overlay, nullptr, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_delete(s_pop.scrim);
    lv_obj_delete(s_pop.card);
    s_pop = Pop{};
}

void run(std::int32_t from, std::int32_t to, std::uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_delete(&s_pop, nullptr);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_pop);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_custom_exec_cb(&a, [](lv_anim_t *an, int32_t v) { grow(an, v); });
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    if (done != nullptr) {
        lv_anim_set_completed_cb(&a, done);
    }
    lv_anim_start(&a);
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
    static char both[300];
    std::snprintf(both, sizeof both, "%s%s%s", s_freeze_report, s_up_report, s_ahead_report);
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
    lv_obj_add_state(source, LV_STATE_USER_1);

    const bool frozen = s_how == PopGlass::Frozen;
    if (frozen) {
        freeze();
    }
    s_open.store(true, std::memory_order_release);
    if (frozen && s_frozen != nullptr) {
        // the overlay plane itself takes the picture as its opaque background, so
        // LVGL starts drawing there and skips everything the picture hides
        lv_obj_set_style_bg_image_src(top, s_frozen, 0);
        lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
        s_pop.scrim = lv_obj_create(top);
        lv_obj_remove_style_all(s_pop.scrim);
        lv_obj_set_size(s_pop.scrim, size::screen_w, size::screen_h);
    } else {
        s_pop.scrim = lv_obj_create(top);
        lv_obj_remove_style_all(s_pop.scrim);
        lv_obj_set_size(s_pop.scrim, size::screen_w, size::screen_h);
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
    if (!frozen || s_frozen == nullptr) {
        lv_obj_set_style_blur_backdrop(s_pop.card, true, 0);
        lv_obj_set_style_blur_radius(s_pop.card, 40, 0);
    } else if (s_motion == PopMotion::Rise) {
        bake_shadow(s_pop.to);
#if defined(ESP_PLATFORM)
        s_made_at_gen.store(0, std::memory_order_relaxed);  // a shadow painted in: never this picture again
        s_made_from_us.store(0, std::memory_order_relaxed);
#endif
        lv_obj_set_style_shadow_width(s_pop.card, 0, 0);
    }
    lv_obj_set_style_pad_all(s_pop.card, 0, 0);
    lv_obj_add_flag(s_pop.card, LV_OBJ_FLAG_CLICKABLE);

    s_pop.body = lv_obj_create(s_pop.card);
    lv_obj_remove_style_all(s_pop.body);
    lv_obj_set_size(s_pop.body, w, h);
    lv_obj_set_style_pad_all(s_pop.body, space::xxl, 0);
    lv_obj_remove_flag(s_pop.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pop.body, LV_OBJ_FLAG_IGNORE_LAYOUT);

    grow(nullptr, 0);
    run(0, PROGRESS, motion::pop_ms, motion::spring, nullptr);
    return s_pop.body;
}

void close_popover(bool animate)
{
    if (s_pop.card == nullptr || s_pop.closing) {
        return;
    }
    if (!animate) {
        lv_anim_delete(&s_pop, nullptr);
        finish_close(nullptr);
        return;
    }
    s_pop.closing = true;
    run(PROGRESS, 0, motion::close_ms, lv_anim_path_ease_in, finish_close);
}

// --- notices --------------------------------------------------------------

namespace {
void drop(void *obj, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), y); }

void leave(lv_obj_t *slab)
{
    if (slab == nullptr || lv_obj_has_flag(slab, LV_OBJ_FLAG_USER_4)) {
        return;
    }
    lv_obj_add_flag(slab, LV_OBJ_FLAG_USER_4);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slab);
    lv_anim_set_values(&a, lv_obj_get_y(slab), -lv_obj_get_height(slab) - 40);
    lv_anim_set_exec_cb(&a, drop);
    lv_anim_set_duration(&a, motion::close_ms + 60);
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

    lv_obj_t *mark = well(slab);
    lv_obj_set_size(mark, 76, 76);
    lv_obj_set_style_pad_all(mark, 0, 0);
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

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slab);
    lv_anim_set_values(&a, -H, Y);
    lv_anim_set_exec_cb(&a, drop);
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
