#include "battery.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"
#include "ui.h"

#include <cmath>

namespace battery {
namespace {

constexpr char TAG[] = "battery";

// As often as a charge icon is worth taking the shared I2C bus for.
constexpr TickType_t POLL_INTERVAL = pdMS_TO_TICKS(30000);

// Logs a float, which goes through full newlib printf.
constexpr std::uint32_t TASK_STACK    = 4096;
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

[[noreturn]] void battery_task(void *)
{
    for (;;) {
        power::reassert_charging();

        power::State state{};
        if (power::read(state) == ESP_OK) {
            ESP_LOGI(TAG, "%d%% %.2f V %d mA", state.percent, state.bus_volts,
                     static_cast<int>(std::lround(state.current_amps * 1000.0f)));
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::set_battery(state.present, state.percent, state.charging));
        }
        vTaskDelay(POLL_INTERVAL);
    }
}

}  // namespace

esp_err_t start()
{
    ESP_RETURN_ON_ERROR(power::init(), TAG, "power monitor");

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(battery_task, "battery", TASK_STACK, nullptr,
                                                      TASK_PRIORITY, s_task_stack, &s_task_ctrl,
                                                      TASK_CORE);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

}  // namespace battery
