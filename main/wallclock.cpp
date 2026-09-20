#include "wallclock.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace wallclock {
namespace {

constexpr char TAG[] = "clock";

constexpr TickType_t TICK = pdMS_TO_TICKS(1000);

// An unsynced device comes up in 1970. Anything past this is a real time
// rather than the epoch, without needing to ask SNTP whether it finished.
constexpr int PLAUSIBLE_YEAR = 2024;

constexpr std::uint32_t TASK_STACK    = 3072;
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

std::atomic<bool> s_synced{false};

[[noreturn]] void clock_task(void *)
{
    char last[6] = {};

    for (;;) {
        const std::time_t now = std::time(nullptr);
        std::tm           local{};
        localtime_r(&now, &local);

        if (local.tm_year + 1900 >= PLAUSIBLE_YEAR) {
            char text[6];
            std::strftime(text, sizeof(text), "%H:%M", &local);
            if (!s_synced.exchange(true, std::memory_order_relaxed)) {
                ESP_LOGI(TAG, "time set: %s", text);
            }
            // Only repaint on the minute rolling over, not every second.
            if (std::strncmp(text, last, sizeof(text)) != 0) {
                std::snprintf(last, sizeof(last), "%s", text);
                ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_time(text));
            }
        }
        vTaskDelay(TICK);
    }
}

}  // namespace

esp_err_t start()
{
    TaskHandle_t task = xTaskCreateStaticPinnedToCore(clock_task, "clock", TASK_STACK, nullptr,
                                                      TASK_PRIORITY, s_task_stack, &s_task_ctrl,
                                                      TASK_CORE);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

bool synced()
{
    return s_synced.load(std::memory_order_relaxed);
}

}  // namespace wallclock
