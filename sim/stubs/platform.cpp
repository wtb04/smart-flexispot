// The clock and delays ESP-IDF gives the firmware.
#include "esp_timer.h"
#include "freertos/task.h"

#include <chrono>
#include <thread>

namespace {
const auto s_boot = std::chrono::steady_clock::now();
}

int64_t esp_timer_get_time()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - s_boot)
        .count();
}

TickType_t xTaskGetTickCount()
{
    return static_cast<TickType_t>(esp_timer_get_time() / 1000);
}

void vTaskDelay(TickType_t ticks)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
