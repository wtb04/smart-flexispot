#include "board.h"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace board {
namespace {

constexpr char TAG[] = "board";

constexpr TickType_t LCD_RAIL_SETTLE   = pdMS_TO_TICKS(200);
constexpr TickType_t TOUCH_RAIL_SETTLE = pdMS_TO_TICKS(500);
constexpr int        BRIGHTNESS_PCT    = 80;

constexpr int LVGL_TASK_PRIORITY  = 4;
constexpr int LVGL_TASK_STACK     = 8192;
constexpr int LVGL_TASK_CORE      = 1;
constexpr int LVGL_TICK_PERIOD_MS = 5;
constexpr int LVGL_MAX_SLEEP_MS   = 500;

// bsp_display_start() tells the two Tab5 display revisions apart by probing the
// touch controller, and an ST7123 only answers once the LCD rail has been up
// for a while -- otherwise detection asserts with "Unsupported board version!".
// Raising the rails here first is idempotent. See espressif/esp-bsp#829.
esp_err_t power_up_panel()
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_LCD, true), TAG, "lcd rail");
    vTaskDelay(LCD_RAIL_SETTLE);
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_TOUCH, true), TAG, "touch rail");
    vTaskDelay(TOUCH_RAIL_SETTLE);
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
        .buffer_size   = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags = {
            .buff_dma    = true,
            .buff_spiram = false,
            // Rotation off the CPU: the port routes this through the P4's PPA
            // when CONFIG_LVGL_PORT_ENABLE_PPA is set.
            .sw_rotate   = true,
        },
    };

    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(disp != nullptr, ESP_FAIL, TAG, "display start");
    *out_disp = disp;
    return ESP_OK;
}

}  // namespace

esp_err_t init()
{
    ESP_RETURN_ON_ERROR(power_up_panel(), TAG, "panel power");

    lv_display_t *disp = nullptr;
    ESP_RETURN_ON_ERROR(start_display(&disp), TAG, "display");

    // Panel is natively 720x1280 portrait; the Tab5 is used landscape.
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);

    ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(BRIGHTNESS_PCT), TAG, "brightness");
    return ESP_OK;
}

}  // namespace board
