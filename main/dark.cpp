#include "dark.h"

#include "app_state.h"
#include "board.h"
#include "imu.h"
#include "network.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Dark, the panel's picture and its stream stop and the CPU slows: a third
// less from the battery, measured. This panel's touch is timed off the stream
// and stops with it, so a knock on the glass, which the IMU feels, lights it
// instead: a light tap jumps 0.9 g between two readings, stillness a tenth of
// that. Without an IMU that answers, dark is only the backlight and the touch
// wakes it, as before.
namespace dark {
namespace {
constexpr char       TAG[]       = "dark";
constexpr float      KNOCK_G     = 0.3f;
constexpr TickType_t LOOK_EVERY  = pdMS_TO_TICKS(150);  // what a knock waits for, at most
constexpr TickType_t UNTIL_OFF   = pdMS_TO_TICKS(50);
constexpr uint32_t   TASK_STACK  = 3072;
constexpr UBaseType_t TASK_PRIORITY = 4;

TaskHandle_t s_task = nullptr;

// Asleep until lit again, by a knock or by anything else; then awake, the
// IMU back to its slow readings.
void sleep_until_lit()
{
    if (imu::watch_knocks() != ESP_OK) {
        ESP_LOGW(TAG, "no knocks to wake it: dark with the touch on");
        return;
    }
    // Told dark before the display is: it sleeps once it is off.
    while (!board::display_sleep()) {
        if (app::get(app::Fact::ScreenOn)) {
            imu::stop_watching_knocks();
            return;
        }
        vTaskDelay(UNTIL_OFF);
    }
    ESP_LOGI(TAG, "asleep");
    while (!app::get(app::Fact::ScreenOn)) {
        vTaskDelay(LOOK_EVERY);
        if (const float jolt = imu::sharpest_jolt(); jolt > KNOCK_G && !app::get(app::Fact::ScreenOn)) {
            ESP_LOGI(TAG, "knocked, %.2f g", jolt);
            network::set_screen(true);
        }
    }
    imu::stop_watching_knocks();
}

void task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!app::get(app::Fact::ScreenOn)) {
            sleep_until_lit();
        }
    }
}
}  // namespace

esp_err_t start()
{
    ESP_RETURN_ON_FALSE(xTaskCreate(task, TAG, TASK_STACK, nullptr, TASK_PRIORITY, &s_task) == pdPASS, ESP_ERR_NO_MEM,
                        TAG, "task");
    app::watch(app::Fact::ScreenOn, [](bool) { xTaskNotifyGive(s_task); });
    if (!app::get(app::Fact::ScreenOn)) {
        xTaskNotifyGive(s_task);
    }
    return ESP_OK;
}

}  // namespace dark
