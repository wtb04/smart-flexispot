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

// As often as a charge icon is worth taking the shared I2C bus for.
constexpr TickType_t POLL_INTERVAL = pdMS_TO_TICKS(30000);

// Where charging starts again after the pack has reported itself full. The
// charger would otherwise hold it at its setpoint indefinitely, and a pack left
// sitting at the top of its range is the one that ages fastest. Well below the
// full threshold on purpose: anything closer and it would relax into a resume,
// charge for a minute, report full, and do it again for ever.
constexpr float RESUME_VOLTS = 8.00f;

// Logs a float, which goes through full newlib printf.
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
            // TEMPORARY: both IO expanders' input registers, logged every poll.
            // The board exposes no documented battery-detect line -- M5's own
            // BSP declares no battery support at all -- but six pins on the
            // expander that carries the charger are unaccounted for. If one of
            // them tracks the pack, presence becomes something to read rather
            // than something to infer. Pull the pack out and watch for a bit
            // that flips.
            power::log_expanders();

            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::set_battery(state.present, state.percent, state.charging));

            // No sense driving a charger into an empty socket: that is what
            // makes the sense node swing, and it is what the pack detection has
            // to see through.
            bool present = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(power::probe_pack(present));

            // Full latches the charger off and only a real fall clears it, so
            // the two thresholds are what stops it cycling.
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
    // init() only reads; whether the charger runs is the stored setting, and
    // set_charging refuses a pack too flat to take it.
    ESP_ERROR_CHECK_WITHOUT_ABORT(power::set_charging(settings::enabled(settings::Key::Charging)));

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(battery_task, "battery", TASK_STACK, nullptr,
                                                      TASK_PRIORITY, s_task_stack, &s_task_ctrl,
                                                      TASK_CORE);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

}  // namespace battery
