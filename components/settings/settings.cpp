#include "settings.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <algorithm>
#include <atomic>
#include <iterator>

namespace settings {
namespace {

constexpr char TAG[]       = "settings";
constexpr char NAMESPACE[] = "panel";

struct Spec {
    const char *name;  // NVS keys are limited to 15 characters
    int         fallback;
    int         low;
    int         high;
};

constexpr Spec SPECS[] = {
    {"brightness", 80, 0, 100},
    {"charging", 1, 0, 1},
    {"volume", 60, 0, 100},
    {"presence_gate", 1, 0, 1},
    {"desk_over_ble", 1, 0, 1},
    // Zero until a colour has been chosen, which leaves the panel on its own.
    {"accent", 0, 0, 0xffffff},
    {"rail_right", 0, 0, 1},
};
constexpr int COUNT = static_cast<int>(Key::Count);
static_assert(std::size(SPECS) == COUNT, "every key needs a spec");

std::atomic<int>           s_value[COUNT];
std::atomic<std::uint32_t> s_dirty{0};

// Long enough that dragging the brightness slider from one end to the other is
// a single write, short enough that pulling the power straight after a change
// keeps it.
constexpr TickType_t SETTLE = pdMS_TO_TICKS(2000);

constexpr std::uint32_t TASK_STACK    = 2048;  // measured: uses 0.3 KB
constexpr UBaseType_t   TASK_PRIORITY = 1;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

void commit()
{
    const std::uint32_t dirty = s_dirty.exchange(0, std::memory_order_relaxed);
    if (dirty == 0) {
        return;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open storage");
        return;
    }
    for (int i = 0; i < COUNT; ++i) {
        if ((dirty & (1u << i)) != 0) {
            nvs_set_i32(handle, SPECS[i].name, s_value[i].load(std::memory_order_relaxed));
        }
    }
    if (nvs_commit(handle) == ESP_OK) {
        ESP_LOGI(TAG, "stored 0x%02x", static_cast<unsigned>(dirty));
    }
    nvs_close(handle);
}

[[noreturn]] void settings_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Let a burst finish before writing, so the flash sees one write.
        vTaskDelay(SETTLE);
        commit();
    }
}

}  // namespace

esp_err_t load()
{
    nvs_handle_t handle = 0;
    const bool   opened = nvs_open(NAMESPACE, NVS_READONLY, &handle) == ESP_OK;
    for (int i = 0; i < COUNT; ++i) {
        std::int32_t stored = SPECS[i].fallback;
        if (opened && nvs_get_i32(handle, SPECS[i].name, &stored) != ESP_OK) {
            stored = SPECS[i].fallback;
        }
        s_value[i].store(std::clamp<int>(stored, SPECS[i].low, SPECS[i].high),
                         std::memory_order_relaxed);
    }
    if (opened) {
        nvs_close(handle);
    }

    s_task = xTaskCreateStatic(settings_task, "settings", TASK_STACK, nullptr, TASK_PRIORITY,
                               s_task_stack, &s_task_ctrl);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

int get(Key key)
{
    const int index = static_cast<int>(key);
    return index >= 0 && index < COUNT ? s_value[index].load(std::memory_order_relaxed) : 0;
}

bool enabled(Key key)
{
    return get(key) != 0;
}

void set(Key key, int value)
{
    const int index = static_cast<int>(key);
    if (index < 0 || index >= COUNT) {
        return;
    }
    const int clamped = std::clamp(value, SPECS[index].low, SPECS[index].high);
    if (s_value[index].exchange(clamped, std::memory_order_relaxed) == clamped) {
        return;
    }
    s_dirty.fetch_or(1u << index, std::memory_order_relaxed);
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace settings
