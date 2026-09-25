#include "orientation.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "settings.h"

#include <cmath>

namespace orientation {
namespace {
constexpr char TAG[] = "orientation";

constexpr TickType_t TICK = pdMS_TO_TICKS(100);

// Standing, not lying on the desk or held at an angle: most of gravity along
// the screen's long-side axis.
constexpr float STANDING_G = 0.6f;

// Turned only once it has stayed the other way up this long, so a panel being
// picked up or knocked does not spin the screen.
constexpr int SETTLED_TICKS = 5;

constexpr std::uint32_t TASK_STACK    = 3072;
constexpr UBaseType_t   TASK_PRIORITY = 1;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

bool standing(float x, float z)
{
    return std::fabs(x) >= STANDING_G && std::fabs(x) > std::fabs(z);
}

[[noreturn]] void orientation_task(void *)
{
    int other_way = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, TICK);
        if (!settings::enabled(settings::Key::OrientAuto)) {
            other_way = 0;
            continue;
        }
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (!imu::gravity(x, y, z) || !standing(x, z)) {
            other_way = 0;
            continue;
        }
        const int  upright = settings::get(settings::Key::OrientSign);
        const bool flipped = (x > 0.0f ? 1 : -1) != upright;
        if (flipped == settings::enabled(settings::Key::Flipped)) {
            other_way = 0;
            continue;
        }
        if (++other_way < SETTLED_TICKS) {
            continue;
        }
        other_way = 0;
        ESP_LOGI(TAG, "stood %s (x %.2f g)", flipped ? "the other way up" : "upright", x);
        board::set_flipped(flipped);
        // Kept, so the next boot starts the way it last stood.
        settings::set(settings::Key::Flipped, flipped);
    }
}

}  // namespace

esp_err_t start()
{
    s_task = xTaskCreateStaticPinnedToCore(orientation_task, "orientation", TASK_STACK, nullptr,
                                           TASK_PRIORITY, s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void calibrate_and_refresh()
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (imu::gravity(x, y, z) && standing(x, z)) {
        // Somebody just pressed Auto, so the screen is the right way up for them.
        const bool flipped = settings::enabled(settings::Key::Flipped);
        const int  sign    = x > 0.0f ? 1 : -1;
        settings::set(settings::Key::OrientSign, flipped ? -sign : sign);
        ESP_LOGI(TAG, "upright is x %s", (flipped ? -sign : sign) > 0 ? "positive" : "negative");
    } else {
        ESP_LOGI(TAG, "not standing, keeping what upright was taken to be");
    }
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace orientation
