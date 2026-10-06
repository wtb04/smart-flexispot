#include "board.h"

#include "bsp/esp-bsp.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/axi_icm_ll.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"
#include "soc/dw_gdma_struct.h"
#include "lvgl_private.h"  // the display's areas to redraw; the version is pinned in dependencies.lock

#include <algorithm>
#include <atomic>
#include <utility>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace board {
namespace {
constexpr char TAG[] = "board";

lv_display_t *s_disp = nullptr;

constexpr TickType_t LCD_RAIL_SETTLE   = pdMS_TO_TICKS(200);
constexpr TickType_t TOUCH_RAIL_SETTLE = pdMS_TO_TICKS(500);

constexpr int LVGL_TASK_PRIORITY  = 4;
// Measured: a page change over the Setup page's tree leaves 2 KB of an 8 KB
// stack, which is not margin.
constexpr int LVGL_TASK_STACK     = 12288;
constexpr int LVGL_TASK_CORE      = 1;
constexpr int LVGL_TICK_PERIOD_MS = 5;
constexpr int LVGL_MAX_SLEEP_MS   = 500;

constexpr int MAX_BRIGHTNESS = 100;

// bsp_display_start() tells the two Tab5 display revisions apart by probing the
// touch controller, and an ST7123 only answers once the LCD rail has been up for
// a while. Raising the rails here first is idempotent. See espressif/esp-bsp#829.
// Both I/O expanders are asked once at start-up, and one did not answer its
// reset in time: the board package asserted, which on a new firmware sent the
// panel back to the one before. Asked again a few times instead, as its
// checks are off (sdkconfig.defaults) and it says so.
constexpr int        EXPANDER_TRIES = 5;
constexpr TickType_t EXPANDER_RETRY = pdMS_TO_TICKS(20);

esp_err_t wake_expanders()
{
    for (int attempt = 1;; ++attempt) {
        if (bsp_io_expander_init() != nullptr && bsp_io_expander1_init() != nullptr) {
            return ESP_OK;
        }
        if (attempt == EXPANDER_TRIES) {
            return ESP_ERR_TIMEOUT;
        }
        ESP_LOGW(TAG, "an I/O expander did not answer, asking again (%d)", attempt);
        vTaskDelay(EXPANDER_RETRY);
    }
}

esp_err_t power_up_panel()
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(wake_expanders(), TAG, "io expanders");
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_LCD, true), TAG, "lcd rail");
    vTaskDelay(LCD_RAIL_SETTLE);
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_TOUCH, true), TAG, "touch rail");
    vTaskDelay(TOUCH_RAIL_SETTLE);
    return ESP_OK;
}

// The port rotates each rendered area into a buffer of its own, then copies that
// into the one frame buffer the panel is reading from, so a page change arrives
// as a wipe across a few refreshes. Here each frame is rotated straight into a
// second, hidden frame buffer, and the panel swaps to it between two refreshes.
// Before the next frame is drawn, what changed is copied back into the buffer
// that has just been hidden, so both hold the same picture.
#ifndef REMOTE_ENABLED
#define REMOTE_ENABLED 0
#endif


constexpr int        MAX_DIRTY     = 64;  // as LV_INV_BUF_SIZE, in the project's CMakeLists.txt
// Four: the panel takes up a buffer a frame or two after it is asked to (see
// components/esp_lcd), and a frame drawn meanwhile replaces the one asked for
// rather than waiting for it, so drawing goes at its own pace and the panel
// shows the newest at each refresh. With three, the frame after waited for
// the panel every time, and that wait, not drawing, held small changes to 30
// frames a second.
constexpr int        FRAME_BUFFERS = 4;
constexpr std::uint32_t DRAWING_LOCK_MS = 1000;
constexpr TickType_t SWAP_TIMEOUT  = pdMS_TO_TICKS(100);

struct Rect {
    std::uint32_t x, y, w, h;
};

esp_lcd_panel_handle_t s_panel = nullptr;
ppa_client_handle_t    s_ppa   = nullptr;
std::uint8_t          *s_fbs[FRAME_BUFFERS] = {};
SemaphoreHandle_t      s_swapped = nullptr;
// The buffer last handed to the panel, and the one it has been seen to show:
// until they are the same, the one shown before may still be read.
std::atomic<int>       s_asked{0};
std::atomic<int>       s_showing{0};
// Buffers the panel may be reading: the one asked for, and any asked for
// before it that the panel has not been seen past. Only buffers outside this
// may be drawn into.
std::atomic<std::uint32_t> s_unsafe{1};
int                    s_target = -1;  // the one this frame goes into, once it begins

board::FlushTimes s_flush_times;

// For each buffer, where frames since it was last drawn into changed: what it
// is behind by. Past MAX_DIRTY, the whole of it.
Rect s_behind[FRAME_BUFFERS][MAX_DIRTY];
int  s_behind_count[FRAME_BUFFERS] = {};

constexpr std::uint32_t FRAME_PIXELS = BSP_LCD_H_RES * BSP_LCD_V_RES;
constexpr std::uint32_t FRAME_BYTES  = FRAME_PIXELS * sizeof(std::uint16_t);  // RGB565

// What the port's own flush waits on; its last copy may still be under way
// when this one takes over. In IRAM, like frame_done, since the panel's
// interrupts now run through flash writes (sdkconfig.defaults) and the driver
// will take nothing else; the flag is all lv_display_flush_ready() clears. It
// comes only as a copy ends, never inside a flash write, so the display it
// reaches may be in PSRAM.
IRAM_ATTR bool copied(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
{
    if (s_disp != nullptr) {
        s_disp->flushing = 0;
    }
    return false;
}

std::atomic<std::uint32_t> s_refreshes{0};

// A frame is due every 17 ms. The panel's DMA goes round a ring of eight
// frames by itself (components/esp_lcd), and its interrupt only has to keep
// the ring going: held off longer than the ring lasts, the panel goes
// without, which it shows as a flicker of blue. Held off at all, it is noted,
// with when, so the log can say what else was going on.
constexpr std::int64_t     LATE_FRAME_US = 26000;
std::int64_t               s_frame_at_us = 0;
std::atomic<std::uint32_t> s_late_frames{0};
std::atomic<std::int32_t>  s_latest_late_us{0};
std::atomic<std::int64_t>  s_latest_late_at{0};

// Every frame sent, whichever buffer it came from.
IRAM_ATTR bool frame_sent(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
{
    s_refreshes.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t now = esp_timer_get_time();
    if (s_frame_at_us != 0 && now - s_frame_at_us > LATE_FRAME_US) {
        s_latest_late_us.store(static_cast<std::int32_t>(now - s_frame_at_us), std::memory_order_relaxed);
        s_latest_late_at.store(now, std::memory_order_relaxed);
        s_late_frames.fetch_add(1, std::memory_order_relaxed);
    }
    s_frame_at_us = now;
    return false;
}

std::atomic<std::uint32_t> s_swaps{0};

// The buffer last handed over is the one on show, so every other is free. The
// driver calls this only while the panel reads the buffer it was last asked
// for, which is s_asked once the handover has returned: show_back_buffer()
// asks first and notes it after.
IRAM_ATTR bool frame_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
{
    s_swaps.fetch_add(1, std::memory_order_relaxed);
    const int asked = s_asked.load(std::memory_order_relaxed);
    s_showing.store(asked, std::memory_order_relaxed);
    s_unsafe.store(1u << asked, std::memory_order_relaxed);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_swapped, &woken);
    return woken == pdTRUE;
}

esp_err_t copy_rect(const std::uint8_t *from, std::uint8_t *to, const Rect &r,
                    ppa_srm_rotation_angle_t angle, std::uint32_t in_w, std::uint32_t in_h,
                    std::uint32_t in_x, std::uint32_t in_y, std::uint32_t out_x, std::uint32_t out_y)
{
    ppa_srm_oper_config_t op{};
    op.in.buffer          = from;
    op.in.pic_w           = in_w;
    op.in.pic_h           = in_h;
    op.in.block_w         = r.w;
    op.in.block_h         = r.h;
    op.in.block_offset_x  = in_x;
    op.in.block_offset_y  = in_y;
    op.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer         = to;
    op.out.buffer_size    = FRAME_BYTES;
    op.out.pic_w          = BSP_LCD_H_RES;
    op.out.pic_h          = BSP_LCD_V_RES;
    op.out.block_offset_x = out_x;
    op.out.block_offset_y = out_y;
    op.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle     = angle;
    op.scale_x            = 1.0f;
    op.scale_y            = 1.0f;
    op.mode               = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

std::uint64_t area_of(const Rect &r) { return static_cast<std::uint64_t>(r.w) * r.h; }

Rect joined(const Rect &a, const Rect &b)
{
    const std::uint32_t x = std::min(a.x, b.x), y = std::min(a.y, b.y);
    return {x, y, std::max(a.x + a.w, b.x + b.w) - x, std::max(a.y + a.h, b.y + b.h) - y};
}

// A buffer's list of where it is behind, each part copied once: a part that
// overlaps one already there becomes the two together when that copies no
// more than both would, as a moving card's frames mostly cover each other.
void note(Rect *list, int &count, const Rect &r)
{
    if (count == 1 && list[0].w == BSP_LCD_H_RES && list[0].h == BSP_LCD_V_RES) {
        return;  // behind by the whole of it already
    }
    Rect add = r;
    for (int i = 0; i < count;) {
        const Rect both = joined(list[i], add);
        if (area_of(both) <= area_of(list[i]) + area_of(add)) {
            add            = both;
            list[i]        = list[--count];
            i              = 0;
        } else {
            ++i;
        }
    }
    if (count < MAX_DIRTY) {
        list[count++] = add;
    } else {
        list[0] = {0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES};
        count   = 1;
    }
}

struct Placement {
    Rect                     rect;
    ppa_srm_rotation_angle_t angle;
};
Placement place_on_panel(lv_display_t *disp, const lv_area_t *area);

bool inside(const Rect &part, const Rect &whole)
{
    return part.x >= whole.x && part.y >= whole.y && part.x + part.w <= whole.x + whole.w &&
           part.y + part.h <= whole.y + whole.h;
}

// Whether the frame about to be drawn redraws all of `r` anyway, which LVGL
// knows before the first of it arrives.
bool redrawn(lv_display_t *disp, const Rect &r)
{
    for (std::uint32_t i = 0; i < disp->inv_p; ++i) {
        if (!disp->inv_area_joined[i] && inside(r, place_on_panel(disp, &disp->inv_areas[i]).rect)) {
            return true;
        }
    }
    return false;
}

// Brings `r` of one buffer up to another, unturned: by the CPU, which moves
// PSRAM to PSRAM faster than the PPA, a whole frame in 26 ms rather than 43,
// then out of the cache for the panel's DMA. What the PPA wrote is never
// stale in the cache, as it drops those lines first.
void catch_up(const std::uint8_t *from, std::uint8_t *to, const Rect &r)
{
    constexpr std::size_t PIXEL = sizeof(std::uint16_t);
    const std::size_t     row   = BSP_LCD_H_RES * PIXEL;
    for (std::uint32_t y = r.y; y < r.y + r.h; ++y) {
        const std::size_t at = y * row + r.x * PIXEL;
        std::memcpy(to + at, from + at, r.w * PIXEL);
    }
    const std::size_t first = r.y * row + r.x * PIXEL;
    const std::size_t last  = (r.y + r.h - 1) * row + (r.x + r.w) * PIXEL;
    esp_cache_msync(to + first, last - first, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// `r` brought up to date but for where `drawn` is about to cover it: the
// parts above and below it, and either side of it between.
void catch_up_around(const std::uint8_t *from, std::uint8_t *to, const Rect &r, const Rect &drawn)
{
    const std::uint32_t r_right = r.x + r.w, r_bottom = r.y + r.h;
    const std::uint32_t d_right = drawn.x + drawn.w, d_bottom = drawn.y + drawn.h;
    if (r.x >= d_right || drawn.x >= r_right || r.y >= d_bottom || drawn.y >= r_bottom) {
        catch_up(from, to, r);  // apart from it altogether
        return;
    }
    const std::uint32_t top    = std::max(r.y, drawn.y);
    const std::uint32_t bottom = std::min(r_bottom, d_bottom);
    if (r.y < top) {
        catch_up(from, to, {r.x, r.y, r.w, top - r.y});
    }
    if (bottom < r_bottom) {
        catch_up(from, to, {r.x, bottom, r.w, r_bottom - bottom});
    }
    if (r.x < drawn.x) {
        catch_up(from, to, {r.x, top, drawn.x - r.x, bottom - top});
    }
    if (d_right < r_right) {
        catch_up(from, to, {d_right, top, r_right - d_right, bottom - top});
    }
}

// A buffer the panel cannot be reading, waited for if all of them might be.
int free_buffer()
{
    for (;;) {
        const int           asked  = s_asked.load(std::memory_order_relaxed);
        const std::uint32_t unsafe = s_unsafe.load(std::memory_order_relaxed);
        for (int i = 0; i < FRAME_BUFFERS; ++i) {
            if (i != asked && (unsafe & (1u << i)) == 0) {
                return i;
            }
        }
        const std::int64_t waiting = esp_timer_get_time();
        if (xSemaphoreTake(s_swapped, SWAP_TIMEOUT) != pdTRUE) {
            ESP_LOGW(TAG, "panel did not swap buffers");
            s_unsafe.store(1u << asked, std::memory_order_relaxed);
        }
        s_flush_times.wait_us += esp_timer_get_time() - waiting;
    }
}

// The catching up, done on the other core while LVGL draws: as drawing starts
// it already knows what it will redraw, so which parts of the buffer it goes
// into need bringing up to date, and by the first area it flushes those are
// mostly done. Without the helper, done there and then.
TaskHandle_t      s_catcher     = nullptr;
SemaphoreHandle_t s_caught      = nullptr;
bool              s_catching    = false;
Rect              s_to_catch[MAX_DIRTY];
int               s_to_catch_n  = 0;

void catch_all()
{
    const std::uint8_t *latest = s_fbs[s_asked.load(std::memory_order_relaxed)];
    for (int i = 0; i < s_to_catch_n; ++i) {
        catch_up(latest, s_fbs[s_target], s_to_catch[i]);
    }
}

void catcher(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        catch_all();
        xSemaphoreGive(s_caught);
    }
}

void begin_back(lv_display_t *disp, bool helped)
{
    s_target     = free_buffer();
    s_to_catch_n = 0;
    for (int i = 0; i < s_behind_count[s_target]; ++i) {
        if (!redrawn(disp, s_behind[s_target][i])) {
            s_to_catch[s_to_catch_n++] = s_behind[s_target][i];
        }
    }
    s_behind_count[s_target] = 0;
    if (helped && s_catcher != nullptr && s_to_catch_n > 0) {
        s_catching = true;
        xTaskNotifyGive(s_catcher);
    } else {
        const std::int64_t catching = esp_timer_get_time();
        catch_all();
        s_flush_times.catch_up_us += esp_timer_get_time() - catching;
    }
}

void render_starts(lv_event_t *e)
{
    if (s_target < 0) {
        begin_back(static_cast<lv_display_t *>(lv_event_get_target(e)), true);
    }
}

// The first area of a frame: the buffer it goes into, brought up to the last
// frame only where this frame does not draw anew, since a whole screen took
// 43 ms.
void prepare_back(lv_display_t *disp)
{
    if (s_target < 0) {
        begin_back(disp, false);
    }
    if (s_catching) {
        const std::int64_t catching = esp_timer_get_time();
        xSemaphoreTake(s_caught, portMAX_DELAY);
        s_catching = false;
        s_flush_times.catch_up_us += esp_timer_get_time() - catching;
    }
}

// Where the area lands on the panel, which is portrait. The PPA turns
// anticlockwise, as LVGL's rotations count.
Placement place_on_panel(lv_display_t *disp, const lv_area_t *area)
{
    const std::int32_t hres = lv_display_get_horizontal_resolution(disp);
    const std::int32_t vres = lv_display_get_vertical_resolution(disp);
    const auto         w    = static_cast<std::uint32_t>(lv_area_get_width(area));
    const auto         h    = static_cast<std::uint32_t>(lv_area_get_height(area));

    std::int32_t x = area->x1;
    std::int32_t y = area->y1;
    Placement    out{{0, 0, w, h}, PPA_SRM_ROTATION_ANGLE_0};
    switch (lv_display_get_rotation(disp)) {
        case LV_DISPLAY_ROTATION_90:
            out.angle  = PPA_SRM_ROTATION_ANGLE_90;
            x          = area->y1;
            y          = hres - area->x2 - 1;
            out.rect.w = h;
            out.rect.h = w;
            break;
        case LV_DISPLAY_ROTATION_180:
            out.angle = PPA_SRM_ROTATION_ANGLE_180;
            x         = hres - area->x2 - 1;
            y         = vres - area->y2 - 1;
            break;
        case LV_DISPLAY_ROTATION_270:
            out.angle  = PPA_SRM_ROTATION_ANGLE_270;
            x          = vres - area->y2 - 1;
            y          = area->x1;
            out.rect.w = h;
            out.rect.h = w;
            break;
        default:
            break;
    }
    out.rect.x = static_cast<std::uint32_t>(x);
    out.rect.y = static_cast<std::uint32_t>(y);
    return out;
}

// Handing the driver one of its own buffers makes it the one shown from a
// frame or two on; the one-line area keeps its cache flush to a line. One
// asked for and not yet taken up is simply replaced: it stays unsafe until
// the panel is seen on the newer one. The order matters: unsafe first, so
// it is never free while it may be read; asked after, so frame_done only
// ever names the buffer the driver now has.
void show_back_buffer()
{
    s_unsafe.fetch_or(1u << s_target, std::memory_order_relaxed);
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 1, 1, s_fbs[s_target]);
    s_asked.store(s_target, std::memory_order_relaxed);
    s_target = -1;
}

// What this frame drew, which every other buffer is now behind by.
void note_drawn(const Rect &r)
{
    for (int i = 0; i < FRAME_BUFFERS; ++i) {
        if (i != s_target) {
            note(s_behind[i], s_behind_count[i], r);
        }
    }
}

void flush_rotated(lv_display_t *disp, const lv_area_t *area, std::uint8_t *pixels)
{
    prepare_back(disp);

    const auto      w     = static_cast<std::uint32_t>(lv_area_get_width(area));
    const auto      h     = static_cast<std::uint32_t>(lv_area_get_height(area));
    const Placement place = place_on_panel(disp, area);

    const Rect         in{0, 0, w, h};
    const std::int64_t turning = esp_timer_get_time();
    copy_rect(pixels, s_fbs[s_target], in, place.angle, w, h, 0, 0, place.rect.x, place.rect.y);
    s_flush_times.rotate_us += esp_timer_get_time() - turning;
    s_flush_times.areas += 1;
    s_flush_times.pixels += static_cast<std::uint64_t>(w) * h;
    note_drawn(place.rect);

    if (lv_display_flush_is_last(disp)) {
        s_flush_times.frames += 1;
        show_back_buffer();
    }
    lv_display_flush_ready(disp);
}

bool s_drawing = true;

// Dark, LVGL draws nothing, and lit it draws the whole screen again. The video
// stream itself goes on: this panel's touch is timed off it, and stopping it
// left the picture torn and the touch waking the screen by itself. Under the
// LVGL lock, which is recursive, so also from its own task.
void set_drawing(bool on)
{
    if (on == s_drawing || s_disp == nullptr || !lvgl_port_lock(DRAWING_LOCK_MS)) {
        return;
    }
    s_drawing = on;
    lv_display_enable_invalidation(s_disp, on);
    if (on) {
        lv_obj_invalidate(lv_display_get_screen_active(s_disp));
        lv_obj_invalidate(lv_display_get_layer_top(s_disp));
    }
    lvgl_port_unlock();
}

// esp_lvgl_port 2.9 keeps the panel only in its display context, whose first
// fields are these. The version is pinned in dependencies.lock.
struct PortDisplayHead {
    int                       type;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t    panel;
};

esp_err_t flush_straight_to_panel(lv_display_t *disp)
{
    const auto *port = static_cast<const PortDisplayHead *>(lv_display_get_driver_data(disp));
    ESP_RETURN_ON_FALSE(port != nullptr && port->panel != nullptr, ESP_ERR_INVALID_STATE, TAG, "no panel");
    s_panel = port->panel;
    void *fb0 = nullptr;
    void *fb1 = nullptr;
    void *fb2 = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 3, &fb0, &fb1, &fb2), TAG, "frame buffers");
    s_fbs[0] = static_cast<std::uint8_t *>(fb0);
    s_fbs[1] = static_cast<std::uint8_t *>(fb1);
    s_fbs[2] = static_cast<std::uint8_t *>(fb2);
    // the BSP makes three at most; the fourth as the driver makes its own
    auto *fb3 = static_cast<std::uint8_t *>(heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    ESP_RETURN_ON_FALSE(fb3 != nullptr, ESP_ERR_NO_MEM, TAG, "fourth frame buffer");
    esp_cache_msync(fb3, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_add_frame_buffer(s_panel, fb3), TAG, "fourth frame buffer");
    s_fbs[3] = fb3;
    // The panel starts out on the first; the others hold nothing yet.
    for (int i = 1; i < FRAME_BUFFERS; ++i) {
        note(s_behind[i], s_behind_count[i], {0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES});
    }

    static StaticSemaphore_t swapped;
    s_swapped = xSemaphoreCreateBinaryStatic(&swapped);

    ppa_client_config_t client{};
    client.oper_type             = PPA_OPERATION_SRM;
    client.max_pending_trans_num = 4;
    ESP_RETURN_ON_ERROR(ppa_register_client(&client, &s_ppa), TAG, "ppa");

    esp_lcd_dpi_panel_event_callbacks_t callbacks{};
    callbacks.on_color_trans_done   = copied;
    callbacks.on_frame_buf_complete = frame_done;
    callbacks.on_vsync              = frame_sent;
    s_disp = disp;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &callbacks, nullptr), TAG,
                        "panel callbacks");

    lv_display_set_flush_cb(disp, flush_rotated);
    static StaticSemaphore_t caught;
    s_caught = xSemaphoreCreateBinaryStatic(&caught);
    if (xTaskCreatePinnedToCore(catcher, "catch up", 3072, nullptr, LVGL_TASK_PRIORITY, &s_catcher, 1 - LVGL_TASK_CORE) == pdPASS) {
        lv_display_add_event_cb(disp, render_starts, LV_EVENT_RENDER_START, nullptr);
    }
    return ESP_OK;
}

esp_err_t start_display(lv_display_t **out_disp)
{
    const bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority    = LVGL_TASK_PRIORITY,
            .task_stack       = LVGL_TASK_STACK,
            .task_affinity    = LVGL_TASK_CORE,
            .task_max_sleep_ms = LVGL_MAX_SLEEP_MS,
            .task_stack_caps  = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
            .timer_period_ms  = LVGL_TICK_PERIOD_MS,
        },
        .buffer_size   = FRAME_PIXELS,
        // One: each area is turned onto the panel before LVGL goes on, so a
        // second never drew while the first was sent, and was 1.8 MB of PSRAM.
        .double_buffer = false,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            .sw_rotate   = true,
        },
    };

    // The port registers its own callback, which the panel refuses, as not in
    // IRAM, with an error; flush_straight_to_panel() registers ours instead.
    esp_log_level_set("lcd.dsi", ESP_LOG_NONE);
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    esp_log_level_set("lcd.dsi", ESP_LOG_INFO);
    ESP_RETURN_ON_FALSE(disp != nullptr, ESP_FAIL, TAG, "display start");
    if (lvgl_port_lock(0)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(flush_straight_to_panel(disp));
        lvgl_port_unlock();
    }
    *out_disp = disp;
    return ESP_OK;
}

// The panel reads its frame from PSRAM through the DW-GDMA, some 140 MB/s at
// the ST7121's 70 MHz. Every master starts level with every other, and when
// the PPA and the CPU's cache were both busy with a whole radar frame the
// panel's reads fell behind and it showed blue for a moment: what IDF's DSI
// driver calls an underrun, and says only the interconnect can prevent. Its
// reads go first now; the rest keep taking turns behind them.
constexpr std::uint32_t PANEL_READ_QOS = 15;  // the highest

void put_panel_first()
{
    for (std::uint32_t port = 0; port < 2; ++port) {
        axi_icm_ll_set_dw_gdma_qos_arbiter_prio(port, 0, PANEL_READ_QOS);
    }
}

// The DSI driver says "can't fetch data from external memory fast enough,
// underrun happens" from its interrupt, and only on the console. The ROM's
// printf hands every character to a second channel too, so they are counted
// there; in IRAM, as the interrupt may come while the cache is off.
constexpr char             UNDERRUN[]  = "underrun";
constexpr int              UNDERRUN_LEN = sizeof(UNDERRUN) - 1;
std::atomic<std::uint32_t> s_underruns{0};
int                        s_matched = 0;

IRAM_ATTR void watch_console(char c)
{
    if (c == UNDERRUN[s_matched]) {
        if (++s_matched == UNDERRUN_LEN) {
            s_underruns.fetch_add(1, std::memory_order_relaxed);
            s_matched = 0;
        }
    } else {
        s_matched = c == UNDERRUN[0] ? 1 : 0;
    }
}

}  // namespace

namespace {
constexpr int ZOOM_KEEP_MAX = 16;
struct Kept {
    Rect           at;  // on the panel
    std::uint16_t *pixels = nullptr;
};
Kept s_kept[ZOOM_KEEP_MAX];
int  s_kept_count = 0;

void copy_rows(const std::uint8_t *from, std::size_t from_row, std::uint8_t *to, std::size_t to_row,
               std::uint32_t rows, std::size_t bytes)
{
    for (std::uint32_t i = 0; i < rows; ++i) {
        std::memcpy(to + i * to_row, from + i * from_row, bytes);
    }
}
}  // namespace

inline std::uint16_t blend565(std::uint16_t under, std::uint16_t over, std::uint8_t opa)
{
    const std::uint32_t r = ((over >> 11) * opa + (under >> 11) * (255 - opa)) / 255;
    const std::uint32_t g = (((over >> 5) & 0x3f) * opa + ((under >> 5) & 0x3f) * (255 - opa)) / 255;
    const std::uint32_t b = ((over & 0x1f) * opa + (under & 0x1f) * (255 - opa)) / 255;
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}

// The rings over a zoom's frame, each pixel where it is in the picture, turned
// onto the panel as the frame is; then out of the cache, for the DMA.
void lay_rings(const ZoomFrame &f, std::uint8_t *to)
{
    if (f.ring_at == nullptr || f.ring_count == 0) {
        return;
    }
    const std::int32_t hres  = lv_display_get_horizontal_resolution(s_disp);
    const std::int32_t vres  = lv_display_get_vertical_resolution(s_disp);
    const auto         turn  = lv_display_get_rotation(s_disp);
    auto              *frame = reinterpret_cast<std::uint16_t *>(to);
    for (std::size_t i = 0; i < f.ring_count; ++i) {
        const std::int32_t sx = f.x + static_cast<std::int32_t>(f.ring_at[i] % static_cast<std::uint32_t>(f.w));
        const std::int32_t sy = f.y + static_cast<std::int32_t>(f.ring_at[i] / static_cast<std::uint32_t>(f.w));
        if (sx < f.to.x1 || sx > f.to.x2 || sy < f.to.y1 || sy > f.to.y2) {
            continue;
        }
        std::int32_t px = sx, py = sy;
        switch (turn) {
            case LV_DISPLAY_ROTATION_90:  px = sy; py = hres - sx - 1; break;
            case LV_DISPLAY_ROTATION_180: px = hres - sx - 1; py = vres - sy - 1; break;
            case LV_DISPLAY_ROTATION_270: px = vres - sy - 1; py = sx; break;
            default: break;
        }
        std::uint16_t &pixel = frame[static_cast<std::size_t>(py) * BSP_LCD_H_RES + px];
        pixel                = blend565(pixel, f.ring_ink, f.ring_opa[i]);
    }
    esp_cache_msync(to, FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

esp_err_t zoom_begin(const lv_area_t *keep, int count)
{
    zoom_end();
    // The frame on show is the one the zoom starts from, and is LVGL's last.
    while (s_showing.load(std::memory_order_relaxed) != s_asked.load(std::memory_order_relaxed)) {
        if (xSemaphoreTake(s_swapped, SWAP_TIMEOUT) != pdTRUE) {
            break;
        }
    }
    const std::uint8_t *shown = s_fbs[s_asked.load(std::memory_order_relaxed)];
    esp_cache_msync(const_cast<std::uint8_t *>(shown), FRAME_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);  // as the PPA wrote it
    constexpr std::size_t PIXEL = sizeof(std::uint16_t);
    for (int i = 0; i < count && s_kept_count < ZOOM_KEEP_MAX; ++i) {
        Kept &kept   = s_kept[s_kept_count];
        kept.at      = place_on_panel(s_disp, &keep[i]).rect;
        kept.pixels  = static_cast<std::uint16_t *>(
            heap_caps_malloc(static_cast<std::size_t>(kept.at.w) * kept.at.h * PIXEL, MALLOC_CAP_SPIRAM));
        if (kept.pixels == nullptr) {
            continue;
        }
        copy_rows(shown + (kept.at.y * BSP_LCD_H_RES + kept.at.x) * PIXEL, BSP_LCD_H_RES * PIXEL,
                  reinterpret_cast<std::uint8_t *>(kept.pixels), kept.at.w * PIXEL, kept.at.h, kept.at.w * PIXEL);
        ++s_kept_count;
    }
    return ESP_OK;
}

esp_err_t zoom_frame(const ZoomFrame &f)
{
    if (f.picture == nullptr || s_panel == nullptr || f.scale < 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    s_target = free_buffer();
    // The magnification the PPA can do, and the part of the picture that,
    // magnified so, fills `to` without spilling past it: a pixel or so short at
    // its right and bottom, where the frame before shows through.
    const float        scale = std::floor(f.scale * 16.0f) / 16.0f;
    const std::int32_t to_w  = lv_area_get_width(&f.to);
    const std::int32_t to_h  = lv_area_get_height(&f.to);
    const auto         in_w  = static_cast<std::int32_t>(static_cast<float>(to_w) / scale);
    const auto         in_h  = static_cast<std::int32_t>(static_cast<float>(to_h) / scale);
    const std::int32_t in_x  = std::clamp<std::int32_t>(
        static_cast<std::int32_t>(std::lround(f.cx + (static_cast<float>(f.to.x1 - f.x) - f.cx) / scale)), 0, f.w - in_w);
    const std::int32_t in_y  = std::clamp<std::int32_t>(
        static_cast<std::int32_t>(std::lround(f.cy + (static_cast<float>(f.to.y1 - f.y) - f.cy) / scale)), 0, f.h - in_h);
    const auto out_w = static_cast<std::int32_t>(static_cast<float>(in_w) * scale);
    const auto out_h = static_cast<std::int32_t>(static_cast<float>(in_h) * scale);
    const lv_area_t out_area{f.to.x1, f.to.y1, f.to.x1 + out_w - 1, f.to.y1 + out_h - 1};
    const Placement place = place_on_panel(s_disp, &out_area);

    const std::uint8_t *latest = s_fbs[s_asked.load(std::memory_order_relaxed)];
    const std::int64_t  catching = esp_timer_get_time();
    for (int i = 0; i < s_behind_count[s_target]; ++i) {
        catch_up_around(latest, s_fbs[s_target], s_behind[s_target][i], place.rect);
    }
    s_behind_count[s_target] = 0;
    s_flush_times.catch_up_us += esp_timer_get_time() - catching;

    ppa_srm_oper_config_t op{};
    op.in.buffer          = f.picture;
    op.in.pic_w           = static_cast<std::uint32_t>(f.w);
    op.in.pic_h           = static_cast<std::uint32_t>(f.h);
    op.in.block_w         = static_cast<std::uint32_t>(in_w);
    op.in.block_h         = static_cast<std::uint32_t>(in_h);
    op.in.block_offset_x  = static_cast<std::uint32_t>(in_x);
    op.in.block_offset_y  = static_cast<std::uint32_t>(in_y);
    op.in.srm_cm          = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer         = s_fbs[s_target];
    op.out.buffer_size    = FRAME_BYTES;
    op.out.pic_w          = BSP_LCD_H_RES;
    op.out.pic_h          = BSP_LCD_V_RES;
    op.out.block_offset_x = place.rect.x;
    op.out.block_offset_y = place.rect.y;
    op.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle     = place.angle;
    op.scale_x            = scale;
    op.scale_y            = scale;
    op.mode               = PPA_TRANS_MODE_BLOCKING;
    const std::int64_t turning = esp_timer_get_time();
    const esp_err_t    err     = ppa_do_scale_rotate_mirror(s_ppa, &op);
    s_flush_times.rotate_us += esp_timer_get_time() - turning;
    note_drawn(place.rect);
    lay_rings(f, s_fbs[s_target]);

    constexpr std::size_t PIXEL = sizeof(std::uint16_t);
    for (int i = 0; i < s_kept_count; ++i) {
        const Kept &kept = s_kept[i];
        std::uint8_t *at = s_fbs[s_target] + (kept.at.y * BSP_LCD_H_RES + kept.at.x) * PIXEL;
        copy_rows(reinterpret_cast<const std::uint8_t *>(kept.pixels), kept.at.w * PIXEL, at, BSP_LCD_H_RES * PIXEL,
                  kept.at.h, kept.at.w * PIXEL);
        esp_cache_msync(at, ((kept.at.h - 1) * BSP_LCD_H_RES + kept.at.w) * PIXEL,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        note_drawn(kept.at);
    }
    s_flush_times.frames += 1;
    show_back_buffer();
    return err;
}

void zoom_end()
{
    for (int i = 0; i < s_kept_count; ++i) {
        heap_caps_free(s_kept[i].pixels);
        s_kept[i].pixels = nullptr;
    }
    s_kept_count = 0;
}

esp_err_t shrink_shown(std::uint8_t *out, std::size_t out_bytes, std::uint32_t w, std::uint32_t h)
{
    ESP_RETURN_ON_FALSE(s_panel != nullptr && s_disp != nullptr, ESP_ERR_INVALID_STATE, TAG, "no panel");
    // a client of its own: this runs off LVGL's task, and a client takes one job
    // at a time, so sharing the flush's refused one of them now and then
    static ppa_client_handle_t side = nullptr;
    if (side == nullptr) {
        ppa_client_config_t client{};
        client.oper_type             = PPA_OPERATION_SRM;
        client.max_pending_trans_num = 4;
        ESP_RETURN_ON_ERROR(ppa_register_client(&client, &side), TAG, "ppa");
    }
    while (s_showing.load(std::memory_order_relaxed) != s_asked.load(std::memory_order_relaxed)) {
        if (xSemaphoreTake(s_swapped, SWAP_TIMEOUT) != pdTRUE) {
            break;
        }
    }
    ppa_srm_oper_config_t op{};
    op.in.buffer  = s_fbs[s_asked.load(std::memory_order_relaxed)];
    op.in.pic_w   = BSP_LCD_H_RES;
    op.in.pic_h   = BSP_LCD_V_RES;
    op.in.block_w = BSP_LCD_H_RES;
    op.in.block_h = BSP_LCD_V_RES;
    op.in.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer      = out;
    op.out.buffer_size = out_bytes;
    op.out.pic_w       = w;
    op.out.pic_h       = h;
    op.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;
    // the turn back from the panel's way up to LVGL's: the flush's, undone
    switch (lv_display_get_rotation(s_disp)) {
        case LV_DISPLAY_ROTATION_90: op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270; break;
        case LV_DISPLAY_ROTATION_180: op.rotation_angle = PPA_SRM_ROTATION_ANGLE_180; break;
        case LV_DISPLAY_ROTATION_270: op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90; break;
        default: op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0; break;
    }
    const float scale = static_cast<float>(std::max(w, h)) / static_cast<float>(std::max(BSP_LCD_H_RES, BSP_LCD_V_RES));
    op.scale_x = scale;
    op.scale_y = scale;
    op.mode    = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(side, &op);
}

FlushTimes take_flush_times()
{
    return std::exchange(s_flush_times, FlushTimes{});
}

std::uint32_t refreshes()
{
    return s_refreshes.load(std::memory_order_relaxed);
}

LateFrames late_frames()
{
    return {s_late_frames.load(std::memory_order_relaxed), s_latest_late_us.load(std::memory_order_relaxed),
            s_latest_late_at.load(std::memory_order_relaxed)};
}

std::uint32_t underruns()
{
    return s_underruns.load(std::memory_order_relaxed);
}

// The backlight lights whatever the panel shows, and with no video coming the
// panel shows flat blue: the moment the chip restarts, until the first frame
// is drawn. So the pin is driven low and held, which the chip keeps through a
// restart, from before a restart and from the very start until there is a
// picture to light. In IRAM, with nothing but ROM and inline calls, for the
// panic handler.
IRAM_ATTR void hold_dark()
{
    esp_rom_gpio_pad_select_gpio(BSP_LCD_BACKLIGHT);
    esp_rom_gpio_connect_out_signal(BSP_LCD_BACKLIGHT, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_enable(&GPIO, BSP_LCD_BACKLIGHT);
    gpio_ll_set_level(&GPIO, BSP_LCD_BACKLIGHT, 0);
    gpio_ll_hold_en(&GPIO, BSP_LCD_BACKLIGHT);
}

namespace {
bool s_held_through_restart = false;
}  // namespace

void restart_cold()
{
    constexpr std::uint32_t OFF_MS   = 500;
    constexpr std::uint32_t RESET_MS = 100;
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_feature_enable(BSP_FEATURE_WIFI, false));
    hold_dark();
    vTaskDelay(pdMS_TO_TICKS(OFF_MS));
    // The chip's own watchdog, set to reset all of it as power on does, pins
    // and all: a plain restart leaves them as they were.
    wdt_hal_context_t wdt = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_init(&wdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&wdt);
    wdt_hal_config_stage(&wdt, WDT_STAGE0, RESET_MS * rtc_clk_slow_freq_get_hz() / 1000, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&wdt);
    wdt_hal_write_protect_enable(&wdt);
    for (;;) {
    }
}

void dark_from_the_start()
{
    s_held_through_restart = gpio_ll_is_digital_io_hold(&GPIO, BSP_LCD_BACKLIGHT);
    hold_dark();
}

esp_err_t init(bool flipped)
{
    ESP_LOGI(TAG, "backlight %s", s_held_through_restart ? "held dark through the restart" : "not held at the restart");
    esp_register_shutdown_handler(hold_dark);
    ESP_RETURN_ON_ERROR(power_up_panel(), TAG, "panel power");
    put_panel_first();
    esp_rom_install_channel_putc(2, watch_console);

    lv_display_t *disp = nullptr;
    ESP_RETURN_ON_ERROR(start_display(&disp), TAG, "display");

    s_disp = disp;
    set_flipped(flipped);

    ESP_RETURN_ON_ERROR(bsp_display_backlight_off(), TAG, "backlight");
    return ESP_OK;
}

void set_flipped(bool flipped)
{
    // Called from the orientation watcher as well as the screen's own task; the
    // lock is recursive, so taking it there costs nothing.
    if (!lvgl_port_lock(0)) {
        return;
    }
    bsp_display_rotate(s_disp, flipped ? LV_DISPLAY_ROTATION_270 : LV_DISPLAY_ROTATION_90);
    lvgl_port_unlock();
}

esp_err_t set_brightness(int percent)
{
    // The first light is what lets go of the pin held dark since the start.
    if (gpio_ll_is_digital_io_hold(&GPIO, BSP_LCD_BACKLIGHT)) {
        gpio_ll_hold_dis(&GPIO, BSP_LCD_BACKLIGHT);
    }
    return bsp_display_brightness_set(std::clamp(percent, kMinBrightness, MAX_BRIGHTNESS));
}

void set_brightness_percent(int percent)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(set_brightness(percent));
}

// The first light waits for a frame LVGL drew to be on the panel, and a few
// refreshes of it, so that what it lights is the splash and not whatever the
// panel had before.
namespace {
void wait_for_first_frame()
{
    constexpr std::int64_t  FIRST_FRAME_US   = 1000 * 1000;
    constexpr std::uint32_t SETTLE_REFRESHES = 3;
    const std::uint32_t     swaps            = s_swaps.load(std::memory_order_relaxed);
    const std::int64_t      from             = esp_timer_get_time();
    std::uint32_t           shown_at         = 0;
    bool                    shown            = false;
    while (esp_timer_get_time() - from < FIRST_FRAME_US) {
        if (!shown && s_swaps.load(std::memory_order_relaxed) != swaps) {
            shown    = true;
            shown_at = s_refreshes.load(std::memory_order_relaxed);
        }
        if (shown && s_refreshes.load(std::memory_order_relaxed) - shown_at >= SETTLE_REFRESHES) {
            break;
        }
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "first light after %d ms, %s", static_cast<int>((esp_timer_get_time() - from) / 1000),
             shown ? "on a drawn frame" : "with no frame drawn");
}
}  // namespace

esp_err_t display_on(int percent)
{
    set_drawing(true);
    if (gpio_ll_is_digital_io_hold(&GPIO, BSP_LCD_BACKLIGHT)) {
        wait_for_first_frame();
    }
    return set_brightness(percent);
}

// The backlight and nothing else: bsp_display_enter_sleep() also sleeps the touch
// controller, which this board's controller reports as unsupported after having
// already blanked the panel -- and a sleeping controller cannot report the tap
// that is meant to wake it.
esp_err_t display_off()
{
    const esp_err_t err = bsp_display_backlight_off();
    set_drawing(false);
    return err;
}

}  // namespace board
