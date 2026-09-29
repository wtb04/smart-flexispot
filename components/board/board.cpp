#include "board.h"

#include "bsp/esp-bsp.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/axi_icm_ll.h"
#include "soc/dw_gdma_struct.h"
#include "lvgl_private.h"  // the display's areas to redraw; the version is pinned in dependencies.lock

#include <algorithm>
#include <atomic>
#include <cstdint>

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
esp_err_t power_up_panel()
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
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

// A development build logs where a slow frame's hand-over went.
std::int64_t s_wait_us = 0, s_sync_us = 0, s_turn_us = 0;

constexpr int        MAX_DIRTY     = 16;
constexpr int        FRAME_BUFFERS = 2;
constexpr std::uint32_t DRAWING_LOCK_MS = 1000;
constexpr TickType_t SWAP_TIMEOUT  = pdMS_TO_TICKS(100);

struct Rect {
    std::uint32_t x, y, w, h;
};

esp_lcd_panel_handle_t s_panel = nullptr;
ppa_client_handle_t    s_ppa   = nullptr;
std::uint8_t          *s_fbs[FRAME_BUFFERS] = {};
int                    s_back   = 1;  // the panel starts out on the first
SemaphoreHandle_t      s_swapped = nullptr;
bool                   s_swap_pending = false;

// What the frame on show changed, and what the one being drawn has changed so far.
Rect s_shown[MAX_DIRTY];
int  s_shown_count = 0;
Rect s_drawn[MAX_DIRTY];
int  s_drawn_count = 0;

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

// The buffer last handed over is the one on show, so the other is free.
IRAM_ATTR bool frame_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
{
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

void note(Rect *list, int &count, const Rect &r)
{
    if (count < MAX_DIRTY) {
        list[count++] = r;
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

// The first area of a frame waits for the panel to have let go of the buffer
// it last showed, which is at most one refresh, then brings it up to date:
// only where this frame does not draw anew, since a whole screen took 43 ms.
void prepare_back(lv_display_t *disp)
{
    if (!s_swap_pending) {
        return;
    }
    s_swap_pending = false;
    const std::int64_t began = esp_timer_get_time();
    if (xSemaphoreTake(s_swapped, SWAP_TIMEOUT) != pdTRUE) {
        ESP_LOGW(TAG, "panel did not swap buffers");
    }
    const std::int64_t swapped = esp_timer_get_time();
    const std::uint8_t *front = s_fbs[1 - s_back];
    for (int i = 0; i < s_shown_count; ++i) {
        const Rect &r = s_shown[i];
        if (!redrawn(disp, r)) {
            copy_rect(front, s_fbs[s_back], r, PPA_SRM_ROTATION_ANGLE_0, BSP_LCD_H_RES, BSP_LCD_V_RES,
                      r.x, r.y, r.x, r.y);
        }
    }
    s_shown_count = 0;
    s_wait_us += swapped - began;
    s_sync_us += esp_timer_get_time() - swapped;
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

// Handing the driver one of its own buffers makes it the one shown from the
// next refresh; the one-line area keeps its cache flush to a line. A refresh
// ending in between is discarded with the stale ones, which costs at most a
// refresh of waiting rather than drawing into view.
void show_back_buffer()
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 1, 1, s_fbs[s_back]);
    xSemaphoreTake(s_swapped, 0);
    s_back         = 1 - s_back;
    s_swap_pending = true;
    std::copy(s_drawn, s_drawn + s_drawn_count, s_shown);
    s_shown_count = s_drawn_count;
    s_drawn_count = 0;
}

void flush_rotated(lv_display_t *disp, const lv_area_t *area, std::uint8_t *pixels)
{
    prepare_back(disp);

    const auto      w     = static_cast<std::uint32_t>(lv_area_get_width(area));
    const auto      h     = static_cast<std::uint32_t>(lv_area_get_height(area));
    const Placement place = place_on_panel(disp, area);

    const Rect         in{0, 0, w, h};
    const std::int64_t began = esp_timer_get_time();
    copy_rect(pixels, s_fbs[s_back], in, place.angle, w, h, 0, 0, place.rect.x, place.rect.y);
    s_turn_us += esp_timer_get_time() - began;
    note(s_drawn, s_drawn_count, place.rect);

    if (lv_display_flush_is_last(disp)) {
        show_back_buffer();
        constexpr std::int64_t WORTH_SAYING_US = 20000;
        if (REMOTE_ENABLED && s_wait_us + s_sync_us + s_turn_us > WORTH_SAYING_US) {
            ESP_LOGI(TAG, "hand-over: waited %d ms, synced %d, turned %d", static_cast<int>(s_wait_us / 1000),
                     static_cast<int>(s_sync_us / 1000), static_cast<int>(s_turn_us / 1000));
        }
        s_wait_us = s_sync_us = s_turn_us = 0;
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
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, FRAME_BUFFERS, &fb0, &fb1), TAG,
                        "frame buffers");
    s_fbs[0] = static_cast<std::uint8_t *>(fb0);
    s_fbs[1] = static_cast<std::uint8_t *>(fb1);

    static StaticSemaphore_t swapped;
    s_swapped = xSemaphoreCreateBinaryStatic(&swapped);

    ppa_client_config_t client{};
    client.oper_type = PPA_OPERATION_SRM;
    ESP_RETURN_ON_ERROR(ppa_register_client(&client, &s_ppa), TAG, "ppa");

    esp_lcd_dpi_panel_event_callbacks_t callbacks{};
    callbacks.on_color_trans_done   = copied;
    callbacks.on_frame_buf_complete = frame_done;
    callbacks.on_vsync              = frame_sent;
    s_disp = disp;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &callbacks, nullptr), TAG,
                        "panel callbacks");

    lv_display_set_flush_cb(disp, flush_rotated);
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
        .double_buffer = true,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            .sw_rotate   = true,
        },
    };

    lv_display_t *disp = bsp_display_start_with_config(&cfg);
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
// the PPA and the CPU's cache were both busy with a fullscreen radar frame the
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

#if REMOTE_ENABLED
namespace {
struct StallRun {
    int              ms;
    StallProbe       result;
    SemaphoreHandle_t done;
};

// Which DMA channel reads the frame buffers: the one whose source address is in them.
int display_channel()
{
    const auto in_frames = [](std::uint32_t at) {
        for (std::uint8_t *fb : s_fbs) {
            const auto from = reinterpret_cast<std::uintptr_t>(fb);
            if (fb != nullptr && at >= from && at < from + FRAME_BYTES) {
                return true;
            }
        }
        return false;
    };
    for (int ch = 0; ch < 4; ++ch) {
        if (in_frames(DW_GDMA.ch[ch].sar0.val)) {
            return ch;
        }
    }
    return -1;
}

void stall_task(void *arg)
{
    auto &run = *static_cast<StallRun *>(arg);
    StallProbe &out = run.result;
    out.channel     = display_channel();
    out.core        = xPortGetCoreID();
    if (out.channel >= 0) {
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&mux);  // what a heap walk or a realloc does
        const std::int64_t began = esp_timer_get_time();
        std::uint32_t      last  = DW_GDMA.ch[out.channel].sar0.val;
        std::int64_t       since = began;
        out.first               = last;
        for (std::int64_t now = began; now - began < run.ms * 1000; now = esp_timer_get_time()) {
            const std::uint32_t at = DW_GDMA.ch[out.channel].sar0.val;
            if (at != last) {
                ++out.moves;
                out.wraps += at < last ? 1 : 0;
                out.longest_still_us = std::max<std::int32_t>(out.longest_still_us, static_cast<std::int32_t>(now - since));
                since = now;
                last  = at;
            }
            esp_rom_delay_us(50);
        }
        out.longest_still_us = std::max<std::int32_t>(out.longest_still_us,
                                                      static_cast<std::int32_t>(esp_timer_get_time() - since));
        out.last = last;
        portEXIT_CRITICAL(&mux);
    }
    xSemaphoreGive(run.done);
    vTaskDelete(nullptr);
}
}  // namespace

StallProbe probe_stall(int ms, int core)
{
    StallRun run{ms, {}, xSemaphoreCreateBinary()};
    xTaskCreatePinnedToCore(stall_task, "stall", 4096, &run, configMAX_PRIORITIES - 1, nullptr, core);
    xSemaphoreTake(run.done, portMAX_DELAY);
    vSemaphoreDelete(run.done);
    run.result.fb0 = reinterpret_cast<std::uintptr_t>(s_fbs[0]);
    run.result.fb1 = reinterpret_cast<std::uintptr_t>(s_fbs[1]);
    return run.result;
}
#endif

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

esp_err_t init(bool flipped)
{
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
    return bsp_display_brightness_set(std::clamp(percent, kMinBrightness, MAX_BRIGHTNESS));
}

void set_brightness_percent(int percent)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(set_brightness(percent));
}

esp_err_t display_on(int percent)
{
    set_drawing(true);
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
