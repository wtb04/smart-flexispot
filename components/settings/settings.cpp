#include "settings.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace settings {
namespace {
constexpr char TAG[]       = "settings";
constexpr char NAMESPACE[] = "panel";

constexpr int MAX_RGB     = 0xffffff;
constexpr int MAX_PART_MS = 90 * 60 * 1000;  // focus_work's high, the longest part
constexpr int MAX_EPOCH_S = std::numeric_limits<std::int32_t>::max();

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
    {"accent", 0, 0, MAX_RGB},
    {"rail_right", 1, 0, 1},
    {"flipped", 0, 0, 1},
    {"orient_auto", 0, 0, 1},
    // Upright reads x positive on this board, against what M5's demo implies:
    // taken the other way, Auto turned the screen upside down. Replaced when
    // Auto is picked with the panel standing, by whatever is upright then.
    {"orient_sign", 1, -1, 1},
    {"focus_work", 25, 5, 90},
    {"focus_break", 5, 1, 30},
    {"focus_long", 20, 5, 60},
    {"focus_rounds", 4, 1, 8},
    {"focus_phase", 0, 0, 3},
    {"focus_round", 0, 0, 8},
    {"focus_running", 0, 0, 1},
    {"focus_left", 0, 0, MAX_PART_MS},
    {"focus_length", 0, 0, MAX_PART_MS},
    {"focus_ends", 0, 0, MAX_EPOCH_S},
    {"battery_mah", -1, -1, 1900},
    {"radar_km", 80, 20, 160},
};
constexpr int COUNT = static_cast<int>(Key::Count);
static_assert(std::size(SPECS) == COUNT, "every key needs a spec");

std::atomic<int>           s_value[COUNT];
std::atomic<std::uint32_t> s_dirty{0};

constexpr TickType_t SETTLE = pdMS_TO_TICKS(2 * units::kMsPerSecond);

constexpr std::uint32_t TASK_STACK    = 3072;  // whole numbers took 0.3 KB; records go deeper into NVS
constexpr UBaseType_t   TASK_PRIORITY = 1;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];
TaskHandle_t s_task = nullptr;

void commit_slots_now();

void commit()
{
    commit_slots_now();
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
        vTaskDelay(SETTLE);
        commit();
    }
}

}  // namespace

void flush()
{
    commit();
}

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

// ---- Records ----

namespace detail {
namespace {
Slot      *s_slots = nullptr;  // every record, as each was constructed
std::mutex s_slots_lock;

// Waits for the settings task, which writes them with the rest.
void wake_writer()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}
}  // namespace

Slot::Slot(const char *space, const char *key, void *value, std::size_t size, Accept accept)
    : space_(space), key_(key), value_(value), size_(size), accept_(accept)
{
    std::lock_guard<std::mutex> hold(s_slots_lock);
    next_   = s_slots;
    s_slots = this;
}

void Slot::load_locked()
{
    if (loaded_) {
        return;
    }
    loaded_             = true;
    nvs_handle_t handle = 0;
    if (nvs_open(space_, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    std::size_t stored = 0;
    if (nvs_get_blob(handle, key_, nullptr, &stored) == ESP_OK && stored > 0) {
        if (stored == size_ && accept_ == nullptr) {
            nvs_get_blob(handle, key_, value_, &stored);
        } else if (accept_ != nullptr) {
            std::vector<std::uint8_t> bytes(stored);
            if (nvs_get_blob(handle, key_, bytes.data(), &stored) == ESP_OK) {
                accept_(*this, bytes.data(), stored);
            }
        } else {
            ESP_LOGW(TAG, "%s/%s is %u bytes, not %u: kept as it starts", space_, key_,
                     static_cast<unsigned>(stored), static_cast<unsigned>(size_));
        }
    }
    nvs_close(handle);
}

void Slot::store_locked()
{
    dirty_              = false;
    nvs_handle_t handle = 0;
    if (nvs_open(space_, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "cannot open %s", space_);
        return;
    }
    if (nvs_set_blob(handle, key_, value_, size_) != ESP_OK || nvs_commit(handle) != ESP_OK) {
        ESP_LOGW(TAG, "%s/%s not stored", space_, key_);
    } else {
        ESP_LOGI(TAG, "stored %s/%s", space_, key_);
    }
    nvs_close(handle);
}

void Slot::read(void *out)
{
    std::lock_guard<std::mutex> hold(lock_);
    load_locked();
    std::memcpy(out, value_, size_);
}

void Slot::write(const void *in, Write when)
{
    {
        std::lock_guard<std::mutex> hold(lock_);
        load_locked();
        if (std::memcmp(value_, in, size_) == 0 && !dirty_) {
            return;
        }
        std::memcpy(value_, in, size_);
        dirty_ = true;
        if (when == Write::Now) {
            store_locked();
            return;
        }
    }
    wake_writer();
}

void commit_slots()
{
    std::lock_guard<std::mutex> list(s_slots_lock);
    for (Slot *slot = s_slots; slot != nullptr; slot = slot->next_) {
        std::lock_guard<std::mutex> hold(slot->lock_);
        if (slot->dirty_) {
            slot->store_locked();
        }
    }
}
}  // namespace detail

namespace {
void commit_slots_now()
{
    detail::commit_slots();
}
}  // namespace

}  // namespace settings
