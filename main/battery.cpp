#include "battery.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"
#include "settings.h"
#include "ui.h"

#include <cmath>
#include <cstdlib>

namespace battery {
namespace {
constexpr char TAG[] = "battery";

constexpr TickType_t POLL_INTERVAL = pdMS_TO_TICKS(30000);

constexpr float RESUME_VOLTS = 8.00f;

// A reading is logged when it moves this far, rather than every poll: the log
// is for what changed.
constexpr int LOG_STEP_PERCENT = 5;

constexpr std::uint32_t TASK_STACK    = 4096;
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

[[noreturn]] void battery_task(void *)
{
    bool topped_off    = false;
    bool was_present   = false;
    int  logged        = -100;

    for (;;) {
        power::reassert_charging();

        power::State state{};
        if (power::read(state) == ESP_OK) {
            bool present = false;
            ESP_ERROR_CHECK_WITHOUT_ABORT(power::probe_pack(present));

            if (present != was_present || std::abs(state.percent - logged) >= LOG_STEP_PERCENT) {
                if (present) {
                    ESP_LOGI(TAG, "%d%% %.2f V %d mA%s", state.percent, state.bus_volts,
                             static_cast<int>(std::lround(state.current_amps * 1000.0f)),
                             state.full ? " full" : "");
                } else {
                    ESP_LOGI(TAG, "no pack, or one too flat to show (%.2f V)", state.bus_volts);
                }
                was_present = present;
                logged      = state.percent;
            }
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::set_battery(state.present, state.percent, state.charging));

            if (state.full) {
                topped_off = true;
            } else if (state.bus_volts <= RESUME_VOLTS) {
                topped_off = false;
            }

            // What the charger is doing, not what was last asked of it: a pack
            // below the floor is refused, and one refitted afterwards has to be
            // noticed and charged.
            const bool setting = settings::enabled(settings::Key::Charging);
            const bool wanted  = setting && present && !topped_off;
            if (wanted != power::charging_enabled() && power::set_charging(wanted) == ESP_OK) {
                ESP_LOGI(TAG, "charger %s", wanted       ? "on"
                                            : !present   ? "off, nothing to charge"
                                            : topped_off ? "off, pack full"
                                                         : "off");
            }
        }
        ulTaskNotifyTake(pdTRUE, POLL_INTERVAL);
    }
}

}  // namespace

esp_err_t start()
{
    ESP_RETURN_ON_ERROR(power::init(), TAG, "power monitor");

    // The task switches the charger on or off in its first pass, once it has
    // looked at the pack.
    s_task = xTaskCreateStaticPinnedToCore(battery_task, "battery", TASK_STACK, nullptr,
                                           TASK_PRIORITY, s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void refresh()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace battery
