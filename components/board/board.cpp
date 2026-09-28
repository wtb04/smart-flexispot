#include "board.h"

#include "bsp/esp-bsp.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_private.h"  // the display's areas to redraw; the version is pinned in dependencies.lock

#include <algorithm>
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
// when this one takes over.
bool copied(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *disp)
{
    lv_display_flush_ready(static_cast<lv_display_t *>(disp));
    return false;
}

bool frame_done(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
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
    if (xSemaphoreTake(s_swapped, SWAP_TIMEOUT) != pdTRUE) {
        ESP_LOGW(TAG, "panel did not swap buffers");
    }
    const std::uint8_t *front = s_fbs[1 - s_back];
    for (int i = 0; i < s_shown_count; ++i) {
        const Rect &r = s_shown[i];
        if (!redrawn(disp, r)) {
            copy_rect(front, s_fbs[s_back], r, PPA_SRM_ROTATION_ANGLE_0, BSP_LCD_H_RES, BSP_LCD_V_RES,
                      r.x, r.y, r.x, r.y);
        }
    }
    s_shown_count = 0;
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

    const Rect in{0, 0, w, h};
    copy_rect(pixels, s_fbs[s_back], in, place.angle, w, h, 0, 0, place.rect.x, place.rect.y);
    note(s_drawn, s_drawn_count, place.rect);

    if (lv_display_flush_is_last(disp)) {
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
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &callbacks, disp), TAG,
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

}  // namespace

esp_err_t init(bool flipped)
{
    ESP_RETURN_ON_ERROR(power_up_panel(), TAG, "panel power");

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
