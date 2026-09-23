#include "desk.h"

#include "ble.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loctek.h"
#include "settings.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include "freertos/queue.h"

namespace desk {
namespace {
constexpr char TAG[] = "desk";
constexpr char kConnected[] = "connected";
constexpr char kAsleep[]    = "desk display asleep";
constexpr char kNoReply[]   = "disconnected";
constexpr char kWaking[]    = "waking desk";

constexpr int WIRING_FAULT_AFTER_WAKES = 3;

constexpr TickType_t SUPERVISE_TICK = pdMS_TO_TICKS(200);
constexpr TickType_t LINK_TIMEOUT = pdMS_TO_TICKS(3000);
constexpr TickType_t WAKE_RETRY = pdMS_TO_TICKS(5000);

constexpr std::uint32_t TASK_STACK    = 4096;  // measured: uses 1.8 KB
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

struct PresetCommand {
    int  index;
    bool store;
};

constexpr UBaseType_t PRESET_QUEUE_LEN = 4;
StaticQueue_t s_preset_queue_ctrl;
PresetCommand s_preset_queue_storage[PRESET_QUEUE_LEN];
QueueHandle_t s_preset_queue = nullptr;

constexpr char NVS_NAMESPACE[] = "desk";
constexpr char NVS_PRESETS[]   = "presets2";

constexpr int DEPARTED_MM = 8;

constexpr TickType_t SETTLE_TIME = pdMS_TO_TICKS(1500);
constexpr TickType_t STILL_TIME = pdMS_TO_TICKS(350);

constexpr TickType_t COMMAND_GRACE = pdMS_TO_TICKS(4000);

constexpr TickType_t LEARN_TIMEOUT = pdMS_TO_TICKS(45000);

int  s_preset_mm[ui::kPresetCount] = {-1, -1, -1, -1, -1, -1};
int  s_learning                    = -1;
int  s_learn_from                  = -1;
TickType_t s_learn_started         = 0;
bool s_presets_dirty               = false;
std::atomic<int> s_active_preset{-1};

const char *preset_name(int index)
{
    switch (index) {
        case 0:  return "preset_1";
        case 1:  return "preset_2";
        case 2:  return "stand";
        case 3:  return "sit";
        case 4:  return "preset_5";
        case 5:  return "preset_6";
        default: return "none";
    }
}

void load_presets()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    // A blob from before presets 5 and 6 holds four; those are kept.
    int         stored[ui::kPresetCount];
    std::size_t size = sizeof(stored);
    const bool  read = nvs_get_blob(handle, NVS_PRESETS, stored, &size) == ESP_OK &&
                      size % sizeof(int) == 0 && size <= sizeof(stored);
    for (int i = 0; i < ui::kPresetCount; ++i) {
        s_preset_mm[i] = read && static_cast<std::size_t>(i) < size / sizeof(int) ? stored[i] : -1;
    }
    nvs_close(handle);
}

void save_presets()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(handle, NVS_PRESETS, s_preset_mm, sizeof(s_preset_mm)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

void remember_preset(int index, int height_mm)
{
    if (index < 0 || index >= ui::kPresetCount || height_mm < 0 ||
        s_preset_mm[index] == height_mm) {
        return;
    }
    s_preset_mm[index] = height_mm;
    s_presets_dirty    = true;
    ESP_LOGI(TAG, "preset %d is %d mm", index + 1, height_mm);
}

// The box lands on its own presets exactly; the panel's two within a centimetre.
constexpr int OWN_PRESET_SLACK_MM = 12;

void publish_active(int height_mm, bool linked, bool moving)
{
    int standing_at = -1;
    for (int i = 0; i < ui::kPresetCount; ++i) {
        const int  slack  = i >= ui::kBoxPresets ? OWN_PRESET_SLACK_MM : 0;
        const bool active = linked && !moving && height_mm >= 0 && s_preset_mm[i] >= 0 &&
                            std::abs(height_mm - s_preset_mm[i]) <= slack;
        if (active) {
            standing_at = i;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_preset_active(i, active));
    }
    s_active_preset.store(standing_at, std::memory_order_relaxed);
}

void clear_active()
{
    s_active_preset.store(-1, std::memory_order_relaxed);
    for (int i = 0; i < ui::kPresetCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_preset_active(i, false));
    }
}

std::atomic<int>  s_height_mm{-1};

bool s_over_ble = false;

bool over_ble()
{
    return s_over_ble;
}

TickType_t s_commanded_at   = 0;  // zero when nothing is expected to move
int        s_commanded_from = -1;

std::atomic<bool> s_linked{false};
std::atomic<int>  s_motion{0};  // -1 down, 0 idle, +1 up

// Presets 5 and 6 are the panel's own, since the control box has four: the desk
// is held up or down until the height reports say it is nearly there, stopping
// short for the few millimetres it runs on.
constexpr int        DRIVE_STOP_EARLY_MM = 8;
constexpr TickType_t DRIVE_TIMEOUT       = pdMS_TO_TICKS(40000);
std::atomic<int>     s_drive_to{-1};  // millimetres, or -1 when not driving
TickType_t           s_drive_since = 0;
int                  s_drive_dir   = 0;

void drive(int direction)
{
    if (direction == s_drive_dir) {
        return;
    }
    s_drive_dir = direction;
    s_motion.store(direction, std::memory_order_relaxed);
    if (over_ble()) {
        ble::desk::hold(direction > 0   ? deskproto::Motion::Up
                        : direction < 0 ? deskproto::Motion::Down
                                        : deskproto::Motion::Idle);
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(direction > 0   ? loctek::Move::Up
                                                           : direction < 0 ? loctek::Move::Down
                                                                           : loctek::Move::Stop));
    }
}

void stop_driving()
{
    if (s_drive_to.exchange(-1, std::memory_order_relaxed) >= 0) {
        drive(0);
    }
}

void keep_driving(TickType_t now)
{
    const int target = s_drive_to.load(std::memory_order_relaxed);
    if (target < 0) {
        return;
    }
    const int height = s_height_mm.load(std::memory_order_relaxed);
    if (!s_linked.load(std::memory_order_relaxed) || now - s_drive_since > DRIVE_TIMEOUT) {
        ESP_LOGW(TAG, "stopped short of %d mm", target);
        stop_driving();
        return;
    }
    if (height < 0) {
        return;
    }
    const int away = target - height;
    if (std::abs(away) <= DRIVE_STOP_EARLY_MM ||
        (s_drive_dir > 0 && away < 0) || (s_drive_dir < 0 && away > 0)) {
        ESP_LOGI(TAG, "at %d mm for %d mm", height, target);
        stop_driving();
        return;
    }
    drive(away > 0 ? 1 : -1);
}

void run_preset(const PresetCommand &cmd)
{
    if (cmd.index < 0 || cmd.index >= ui::kPresetCount) {
        return;
    }
    stop_driving();
    if (cmd.index >= ui::kBoxPresets) {
        const int height = s_height_mm.load(std::memory_order_relaxed);
        if (cmd.store) {
            remember_preset(cmd.index, height);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("", "Preset saved", "success", 2500));
        } else if (s_preset_mm[cmd.index] < 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::notify("", "Hold it to save the height it goes to", "info", 3000));
        } else {
            clear_active();
            s_drive_since = xTaskGetTickCount();
            s_drive_to.store(s_preset_mm[cmd.index], std::memory_order_relaxed);
            keep_driving(s_drive_since);
        }
        return;
    }
    const auto preset = static_cast<loctek::Preset>(cmd.index);
    if (cmd.store) {
        if (over_ble()) {
            ble::desk::store(cmd.index);
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::store_preset(preset));
        }
        remember_preset(cmd.index, s_height_mm.load(std::memory_order_relaxed));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::notify("", "Preset saved", "success", 2500));
    } else {
        clear_active();
        if (over_ble()) {
            ble::desk::preset(cmd.index);
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::goto_preset(preset));
        }
        s_learning        = cmd.index;
        s_learn_from      = s_height_mm.load(std::memory_order_relaxed);
        s_learn_started   = xTaskGetTickCount();
        s_commanded_from  = s_learn_from;
        s_commanded_at    = s_learn_started;
    }
}


void on_height(int height_mm);

std::atomic<int> s_pending_height{-1};
TaskHandle_t     s_height_pump = nullptr;

constexpr std::uint32_t PUMP_STACK = 2048;  // measured: uses 0.7 KB
StaticTask_t            s_pump_ctrl;
StackType_t             s_pump_stack[PUMP_STACK];

void on_proxy_status(int height_mm, bool box_linked, deskproto::Motion motion)
{
    (void)box_linked;
    (void)motion;
    if (height_mm < 0) {
        return;
    }
    s_pending_height.store(height_mm, std::memory_order_relaxed);
    if (s_height_pump != nullptr) {
        xTaskNotifyGive(s_height_pump);
    }
}

[[noreturn]] void height_pump_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int height = s_pending_height.load(std::memory_order_relaxed);
        if (height >= 0) {
            on_height(height);
        }
    }
}

void on_height(int height_mm)
{
    if (s_height_mm.exchange(height_mm, std::memory_order_relaxed) != height_mm) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_height(height_mm));
    }
}

const char *proxy_status()
{
    if (!ble::desk::connected()) {
        return "no proxy in range";
    }
    int               height     = -1;
    bool              box_linked = false;
    deskproto::Motion motion     = deskproto::Motion::Idle;
    if (!ble::desk::last(height, box_linked, motion)) {
        return "proxy not reporting";
    }
    return box_linked ? kConnected : "proxy up, control box silent";
}

const char *link_status(const loctek::Stats &stats, bool link_up, int wake_attempts)
{
    if (link_up) {
        const bool never_read = s_height_mm.load(std::memory_order_relaxed) < 0;
        if (never_read && stats.height_frames > 0 && stats.heights_decoded == 0) {
            return kAsleep;
        }
        return kConnected;
    }
    if (stats.bytes_received == 0) {
        return wake_attempts < WIRING_FAULT_AFTER_WAKES ? kWaking
                                                        : "no data on RX - check wiring";
    }
    if (stats.frames_decoded == 0) {
        return "garbage on RX - check baud and TX/RX";
    }
    return kNoReply;
}

[[noreturn]] void supervisor_task(void *)
{
    const char   *shown      = nullptr;
    loctek::Stats previous   = loctek::stats();
    TickType_t    last_wake     = 0;
    TickType_t    last_frame    = 0;
    int           wake_attempts = 0;
    int           settled_at    = -1;
    TickType_t    settled_since = 0;

    for (;;) {
        PresetCommand cmd;
        if (xQueueReceive(s_preset_queue, &cmd, SUPERVISE_TICK) == pdTRUE) {
            run_preset(cmd);
        }

        const loctek::Stats stats = loctek::stats();
        const TickType_t    now   = xTaskGetTickCount();
        keep_driving(now);
        if (stats.frames_decoded != previous.frames_decoded) {
            last_frame = now;
        }
        previous = stats;

        const bool  link_up = over_ble()
                                  ? std::strcmp(proxy_status(), kConnected) == 0
                                  : (last_frame != 0 && (now - last_frame) < LINK_TIMEOUT);

        const int height = s_height_mm.load(std::memory_order_relaxed);
        if (height != settled_at) {
            settled_at    = height;
            settled_since = now;
        }
        if (s_learning >= 0) {
            if (now - s_learn_started > LEARN_TIMEOUT) {
                s_learning = -1;
            } else if (height >= 0 && height != s_learn_from &&
                       now - settled_since > SETTLE_TIME) {
                remember_preset(s_learning, height);
                s_learning = -1;
            }
        }
        if (s_presets_dirty) {
            s_presets_dirty = false;
            save_presets();
        }
        const bool commanded = s_commanded_at != 0;
        if (commanded) {
            const bool left = height >= 0 && s_commanded_from >= 0 &&
                              std::abs(height - s_commanded_from) > DEPARTED_MM;
            if (left || now - s_commanded_at > COMMAND_GRACE) {
                s_commanded_at = 0;
            }
        }
        const bool moving = commanded || s_motion.load(std::memory_order_relaxed) != 0 ||
                            now - settled_since < STILL_TIME;
        publish_active(height, link_up, moving);

        const char *status = over_ble() ? proxy_status() : link_status(stats, link_up, wake_attempts);

        if (status != shown) {
            shown = status;
            const bool linked = status == kConnected;
            s_linked.store(linked, std::memory_order_relaxed);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_desk_available(linked));
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::set_height(linked ? s_height_mm.load(std::memory_order_relaxed) : -1));
            ESP_LOGI(TAG, "%s", status);
#if CONFIG_LOCTEK_NUDGE_WAKE
            if (status == kAsleep) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::nudge());
            }
#endif
        }
        const bool never_read = s_height_mm.load(std::memory_order_relaxed) < 0;
        // Over Bluetooth a wake needs the proxy there to carry it.
        const bool can_wake = !over_ble() || ble::desk::connected();
        if (never_read && can_wake && status != kConnected &&
            (last_wake == 0 || now - last_wake > WAKE_RETRY)) {
            last_wake = now;
            ++wake_attempts;
            if (over_ble()) {
                ESP_LOGI(TAG, "waking the desk through the proxy");
                ble::desk::wake();
            } else {
                ESP_LOGI(TAG, "waking the desk on the wire");
                ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::wake());
            }
        }
    }
}

}  // namespace

esp_err_t start()
{
    QueueHandle_t presets =
        xQueueCreateStatic(PRESET_QUEUE_LEN, sizeof(PresetCommand),
                           reinterpret_cast<std::uint8_t *>(s_preset_queue_storage),
                           &s_preset_queue_ctrl);
    ESP_RETURN_ON_FALSE(presets != nullptr, ESP_ERR_NO_MEM, TAG, "preset queue");
    s_preset_queue = presets;

    load_presets();

    s_over_ble = settings::enabled(settings::Key::DeskBluetooth);
    ESP_LOGI(TAG, "driving the desk over %s", s_over_ble ? "bluetooth" : "the local wire");
    if (s_over_ble) {
        s_height_pump = xTaskCreateStatic(height_pump_task, "deskht", PUMP_STACK, nullptr,
                                          TASK_PRIORITY, s_pump_stack, &s_pump_ctrl);
        ESP_RETURN_ON_FALSE(s_height_pump != nullptr, ESP_ERR_NO_MEM, TAG, "height pump");
        ble::desk::on_status(on_proxy_status);
    }
    if (!s_over_ble) {
        ESP_RETURN_ON_ERROR(loctek::start(on_height), TAG, "loctek");
    }

    TaskHandle_t task = xTaskCreateStaticPinnedToCore(supervisor_task, "desk", TASK_STACK, nullptr,
                                                      TASK_PRIORITY, s_task_stack, &s_task_ctrl,
                                                      TASK_CORE);
    ESP_RETURN_ON_FALSE(task != nullptr, ESP_ERR_NO_MEM, TAG, "task");

    return ESP_OK;
}

void on_preset(int index, bool store)
{
    if (s_preset_queue == nullptr) {
        return;
    }
    const PresetCommand cmd{index, store};
    xQueueSend(s_preset_queue, &cmd, 0);
}

int height_mm()
{
    return s_height_mm.load(std::memory_order_relaxed);
}

bool linked()
{
    return s_linked.load(std::memory_order_relaxed);
}

const char *active_preset()
{
    return preset_name(s_active_preset.load(std::memory_order_relaxed));
}

const char *active_preset_label()
{
    switch (s_active_preset.load(std::memory_order_relaxed)) {
        case 0:  return "Preset 1";
        case 1:  return "Preset 2";
        case 2:  return "Stand";
        case 3:  return "Sit";
        case 4:  return "Preset 5";
        case 5:  return "Preset 6";
        default: return "Between";
    }
}

int preset_height_mm(int index)
{
    return index >= 0 && index < ui::kPresetCount ? s_preset_mm[index] : -1;
}

const char *motion()
{
    switch (s_motion.load(std::memory_order_relaxed)) {
        case 1:  return "moving_up";
        case -1: return "moving_down";
        default: return "idle";
    }
}

void on_move(ui::Move direction)
{
    stop_driving();  // a hand on the buttons takes over from preset 5 or 6
    loctek::Move move   = loctek::Move::Stop;
    int          motion = 0;
    switch (direction) {
        case ui::Move::Up:   move = loctek::Move::Up;   motion = 1;  break;
        case ui::Move::Down: move = loctek::Move::Down; motion = -1; break;
        case ui::Move::Stop: move = loctek::Move::Stop; motion = 0;  break;
    }
    s_motion.store(motion, std::memory_order_relaxed);
    if (motion != 0) {
        clear_active();
    }
    if (over_ble()) {
        ble::desk::hold(direction == ui::Move::Up     ? deskproto::Motion::Up
                        : direction == ui::Move::Down ? deskproto::Motion::Down
                                                      : deskproto::Motion::Idle);
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(move));
    }
}

}  // namespace desk
