#include "battery.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"
#include "ui.h"
#include "wifi.h"

#include <cmath>

namespace battery {
namespace {

constexpr char TAG[] = "battery";

// A battery does not move quickly, and each read costs two I2C transactions.
constexpr TickType_t POLL_INTERVAL = pdMS_TO_TICKS(2000);

constexpr std::uint32_t TASK_STACK    = 3072;
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
            const int milliamps = static_cast<int>(std::lround(state.current_amps * 1000.0f));
            if (state.present) {
                ESP_LOGI(TAG, "%d%% %.2f V %d mA (shunt %.3f mV)", state.percent,
                         state.bus_volts, milliamps, state.shunt_millivolts);
            } else {
                ESP_LOGI(TAG, "no battery (%.2f V)", state.bus_volts);
            }
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::set_battery(state.present, state.percent, state.bus_volts, milliamps,
                                state.charging));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_wifi(wifi::connected()));
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
