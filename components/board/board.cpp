#include "board.h"

#include "bsp/esp-bsp.h"
#include "driver/ppa.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

// The port rotates each rendered area into a buffer of its own and then copies
// that into the panel's frame buffer: twice through PSRAM, which is what a frame
// costs most here. This rotates straight into the frame buffer instead, and
// without waiting, so the next area renders while the last one is on its way.
ppa_client_handle_t s_ppa = nullptr;
void               *s_frame = nullptr;

bool rotated(ppa_client_handle_t, ppa_event_data_t *, void *user)
{
    lv_display_flush_ready(static_cast<lv_display_t *>(user));
    return false;
}

void flush_rotated(lv_display_t *disp, const lv_area_t *area, std::uint8_t *pixels)
{
    const std::int32_t hres = lv_display_get_horizontal_resolution(disp);
    const std::int32_t vres = lv_display_get_vertical_resolution(disp);
    const std::int32_t w    = lv_area_get_width(area);
    const std::int32_t h    = lv_area_get_height(area);
    const auto rotation     = lv_display_get_rotation(disp);

    // Where the area lands on the panel, which is portrait. The PPA turns
    // anticlockwise, as LVGL's rotations count.
    std::int32_t x = area->x1;
    std::int32_t y = area->y1;
    ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_0;
    switch (rotation) {
        case LV_DISPLAY_ROTATION_90:
            angle = PPA_SRM_ROTATION_ANGLE_90;
            x     = area->y1;
            y     = hres - area->x2 - 1;
            break;
        case LV_DISPLAY_ROTATION_180:
            angle = PPA_SRM_ROTATION_ANGLE_180;
            x     = hres - area->x2 - 1;
            y     = vres - area->y2 - 1;
            break;
        case LV_DISPLAY_ROTATION_270:
            angle = PPA_SRM_ROTATION_ANGLE_270;
            x     = vres - area->y2 - 1;
            y     = area->x1;
            break;
        default:
            break;
    }

    ppa_srm_oper_config_t op{};
    op.in.buffer         = pixels;
    op.in.pic_w          = static_cast<std::uint32_t>(w);
    op.in.pic_h          = static_cast<std::uint32_t>(h);
    op.in.block_w        = static_cast<std::uint32_t>(w);
    op.in.block_h        = static_cast<std::uint32_t>(h);
    op.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer        = s_frame;
    op.out.buffer_size   = BSP_LCD_H_RES * BSP_LCD_V_RES * 2;
    op.out.pic_w         = BSP_LCD_H_RES;
    op.out.pic_h         = BSP_LCD_V_RES;
    op.out.block_offset_x = static_cast<std::uint32_t>(x);
    op.out.block_offset_y = static_cast<std::uint32_t>(y);
    op.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle    = angle;
    op.scale_x           = 1.0f;
    op.scale_y           = 1.0f;
    op.mode              = PPA_TRANS_MODE_NON_BLOCKING;
    op.user_data         = disp;
    if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK) {
        lv_display_flush_ready(disp);
    }
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
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(port->panel, 1, &s_frame), TAG, "frame buffer");

    const ppa_client_config_t client{.oper_type = PPA_OPERATION_SRM};
    ESP_RETURN_ON_ERROR(ppa_register_client(&client, &s_ppa), TAG, "ppa");
    const ppa_event_callbacks_t callbacks{.on_trans_done = rotated};
    ESP_RETURN_ON_ERROR(ppa_client_register_event_callbacks(s_ppa, &callbacks), TAG, "ppa callback");

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
        .buffer_size   = BSP_LCD_H_RES * BSP_LCD_V_RES,
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
    return bsp_display_brightness_set(std::clamp(percent, kMinBrightness, 100));
}

void set_brightness_percent(int percent)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(set_brightness(percent));
}

esp_err_t display_on(int percent)
{
    return set_brightness(percent);
}

// The backlight and nothing else: bsp_display_enter_sleep() also sleeps the touch
// controller, which this board's controller reports as unsupported after having
// already blanked the panel -- and a sleeping controller cannot report the tap
// that is meant to wake it.
esp_err_t display_off()
{
    return bsp_display_backlight_off();
}

}  // namespace board
