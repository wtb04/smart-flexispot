#include "battery.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"
#include "settings.h"
#include "ui.h"

#include <cmath>

namespace battery {
namespace {
constexpr char TAG[] = "battery";

constexpr TickType_t POLL_INTERVAL = pdMS_TO_TICKS(30000);

constexpr float RESUME_VOLTS = 8.00f;

constexpr std::uint32_t TASK_STACK    = 4096;
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

[[noreturn]] void battery_task(void *)
{
    bool charging_on = settings::enabled(settings::Key::Charging);
    bool topped_off  = false;

    for (;;) {
        power::reassert_charging();

        power::State state{};
        if (power::read(state) == ESP_OK) {
            if (state.present) {
                ESP_LOGI(TAG, "%d%% %.2f V %d mA%s", state.percent, state.bus_volts,
                         static_cast<int>(std::lround(state.current_amps * 1000.0f)),
                         state.full ? " full" : "");
            } else {
                ESP_LOGI(TAG, "no pack (%.2f V)", state.bus_volts);
            }

            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::set_battery(state.present, state.percent, state.charging));

            bool present = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(power::probe_pack(present));

            if (state.full) {
                topped_off = true;
            } else if (state.bus_volts <= RESUME_VOLTS) {
                topped_off = false;
            }

            const bool wanted =
                settings::enabled(settings::Key::Charging) && present && !topped_off;
            if (wanted != charging_on && power::set_charging(wanted) == ESP_OK) {
                charging_on = wanted;
                ESP_LOGI(TAG, "charger %s", wanted           ? "on"
                                            : !present       ? "off, nothing to charge"
                                            : topped_off     ? "off, pack full"
                                                             : "off");
            }
        }
        vTaskDelay(POLL_INTERVAL);
    }
}

}  // namespace

esp_err_t start()
{
    ESP_RETURN_ON_ERROR(power::init(), TAG, "power monitor");
    ESP_ERROR_CHECK_WITHOUT_ABORT(power::set_charging(settings::enabled(settings::Key::Charging)));

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(battery_task, "battery", TASK_STACK, nullptr,
                                                      TASK_PRIORITY, s_task_stack, &s_task_ctrl,
                                                      TASK_CORE);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

}  // namespace battery
