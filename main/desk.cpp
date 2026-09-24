#include "desk.h"

#include "ble.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "loctek.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "settings.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

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

constexpr TickType_t STILL_TIME = pdMS_TO_TICKS(350);

constexpr TickType_t COMMAND_GRACE = pdMS_TO_TICKS(4000);


// Home Assistant's move buttons send one message and no release.
constexpr TickType_t NETWORK_HOLD = pdMS_TO_TICKS(1500);
std::atomic<TickType_t> s_network_hold_until{0};  // zero when no such hold is live


int  s_preset_mm[deskproto::kPresetCount] = {-1, -1, -1, -1, -1, -1};
bool s_presets_dirty               = false;
std::atomic<int> s_active_preset{-1};

void load_presets()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    // A blob from before presets 5 and 6 holds four; those are kept.
    int         stored[deskproto::kPresetCount];
    std::size_t size = sizeof(stored);
    const bool  read = nvs_get_blob(handle, NVS_PRESETS, stored, &size) == ESP_OK &&
                      size % sizeof(int) == 0 && size <= sizeof(stored);
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        const bool have = read && static_cast<std::size_t>(i) < size / sizeof(int);
        s_preset_mm[i]  = have && loctek::in_range(stored[i]) ? stored[i] : -1;
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
    if (index < 0 || index >= deskproto::kPresetCount || height_mm < 0 ||
        s_preset_mm[index] == height_mm) {
        return;
    }
    s_preset_mm[index] = height_mm;
    s_presets_dirty    = true;
    ESP_LOGI(TAG, "preset %d is %d mm", index + 1, height_mm);
}

// The box lands on its own presets exactly. The panel's two land as well as the
// display lets them be read, which above a metre is to the centimetre.
constexpr int OWN_PRESET_SLACK_MM = 10;

void publish_active(int height_mm, bool linked, bool moving)
{
    int standing_at = -1;
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        const int  slack  = i >= deskproto::kBoxPresets ? OWN_PRESET_SLACK_MM : 0;
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
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_preset_active(i, false));
    }
}

std::atomic<int> s_height_mm{-1};

bool s_over_ble = false;

bool over_ble()
{
    return s_over_ble;
}

TickType_t s_commanded_at   = 0;  // zero when nothing is expected to move
int        s_commanded_from = -1;

std::atomic<bool> s_linked{false};

// -1 down, 0 idle, +1 up. The hold is what this panel asked for; the rest is
// what the proxy reports, a round trip behind.
std::atomic<int>  s_hold{0};
std::atomic<int>  s_remote_motion{0};
std::atomic<bool> s_remote_driving{false};

int as_int(loctek::Move move)
{
    switch (move) {
        case loctek::Move::Up:   return 1;
        case loctek::Move::Down: return -1;
        case loctek::Move::Stop: break;
    }
    return 0;
}

int motion_now()
{
    if (!over_ble()) {
        return as_int(loctek::motion());
    }
    const int hold = s_hold.load(std::memory_order_relaxed);
    return hold != 0 ? hold : s_remote_motion.load(std::memory_order_relaxed);
}

bool travelling()
{
    return over_ble() ? s_remote_driving.load(std::memory_order_relaxed)
                      : loctek::driving_to() >= 0;
}

int s_travel_index = -1;  // which of the panel's presets is being travelled to

// Presets 5 and 6 are the panel's own, since the control box has four. Whichever
// board holds the wire steers the desk there on each height the box reports.
bool travel_to(int height_mm)
{
    if (s_hold.load(std::memory_order_relaxed) != 0) {
        ESP_LOGW(TAG, "not travelling while a button is held");
        return false;
    }
    if (over_ble()) {
        ble::desk::goto_height(height_mm);
        return true;
    }
    return loctek::goto_height(height_mm) == ESP_OK;
}

void let_go()
{
    s_hold.store(0, std::memory_order_relaxed);
    if (over_ble()) {
        ble::desk::stop();
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(loctek::Move::Stop));
    }
}

void run_preset(const PresetCommand &cmd)
{
    if (cmd.index < 0 || cmd.index >= deskproto::kPresetCount) {
        return;
    }
    const int height = s_height_mm.load(std::memory_order_relaxed);
    if (cmd.store) {
        if (cmd.index < deskproto::kBoxPresets) {
            if (over_ble()) {
                ble::desk::store(cmd.index);
            } else {
                ESP_ERROR_CHECK_WITHOUT_ABORT(
                    loctek::store_preset(static_cast<loctek::Preset>(cmd.index)));
            }
        }
        remember_preset(cmd.index, height);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("", "Preset saved", "success", 2500));
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (cmd.index >= deskproto::kBoxPresets) {
        if (s_preset_mm[cmd.index] < 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::notify("", "Hold it to save the height it goes to", "info", 3000));
            return;
        }
        if (travelling() && s_travel_index == cmd.index) {
            let_go();  // tapping it again is how a travel is cancelled, as with the box's own
            return;
        }
        if (!travel_to(s_preset_mm[cmd.index])) {
            return;
        }
        s_travel_index = cmd.index;
    } else {
        if (over_ble()) {
            ble::desk::preset(cmd.index);
        } else {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                loctek::goto_preset(static_cast<loctek::Preset>(cmd.index)));
        }
    }
    clear_active();
    s_commanded_from = height;
    s_commanded_at   = now;
}

void on_height(int height_mm);

std::atomic<int> s_pending_height{-1};
TaskHandle_t     s_height_pump = nullptr;

constexpr std::uint32_t PUMP_STACK = 2048;  // measured: uses 0.7 KB
StaticTask_t            s_pump_ctrl;
StackType_t             s_pump_stack[PUMP_STACK];

void on_proxy_status(const deskproto::Status &status)
{
    s_remote_motion.store(deskproto::direction_of(status.motion), std::memory_order_relaxed);
    s_remote_driving.store(status.driving, std::memory_order_relaxed);
    if (status.height_mm < 0) {
        return;
    }
    s_pending_height.store(status.height_mm, std::memory_order_relaxed);
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

void on_wire_height(int height_mm)
{
    s_pending_height.store(height_mm, std::memory_order_relaxed);
    if (s_height_pump != nullptr) {
        xTaskNotifyGive(s_height_pump);
    }
}

const char *proxy_status()
{
    if (!ble::desk::connected()) {
        return "no proxy in range";
    }
    deskproto::Status status{};
    if (!ble::desk::last(status)) {
        return "proxy not reporting";
    }
    return status.linked ? kConnected : "proxy up, control box silent";
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
        if (s_presets_dirty) {
            s_presets_dirty = false;
            save_presets();
        }
        const TickType_t network_until = s_network_hold_until.load(std::memory_order_relaxed);
        if (network_until != 0 && static_cast<std::int32_t>(now - network_until) >= 0) {
            s_network_hold_until.store(0, std::memory_order_relaxed);
            ESP_LOGW(TAG, "a move from the network was not asked for again, letting go");
            on_move(ui::Move::Stop);
        }
        const bool commanded = s_commanded_at != 0;
        if (commanded) {
            const bool left = height >= 0 && s_commanded_from >= 0 &&
                              std::abs(height - s_commanded_from) > DEPARTED_MM;
            if (left || now - s_commanded_at > COMMAND_GRACE) {
                s_commanded_at = 0;
            }
        }
        const bool moving = commanded || motion_now() != 0 || travelling() ||
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
    // Heights arrive on the radio host task or the UART receive task; neither
    // may wait on the screen, so the pump carries them over.
    s_height_pump = xTaskCreateStatic(height_pump_task, "deskht", PUMP_STACK, nullptr,
                                      TASK_PRIORITY, s_pump_stack, &s_pump_ctrl);
    ESP_RETURN_ON_FALSE(s_height_pump != nullptr, ESP_ERR_NO_MEM, TAG, "height pump");
    if (s_over_ble) {
        ble::desk::on_status(on_proxy_status);
    } else {
        ESP_RETURN_ON_ERROR(loctek::start(on_wire_height), TAG, "loctek");
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

int active_preset_index()
{
    return s_active_preset.load(std::memory_order_relaxed);
}

const char *active_preset_label()
{
    return deskproto::preset_label(active_preset_index());
}

int preset_height_mm(int index)
{
    return index >= 0 && index < deskproto::kPresetCount ? s_preset_mm[index] : -1;
}

const char *motion()
{
    switch (motion_now()) {
        case 1:  return "moving_up";
        case -1: return "moving_down";
        default: return "idle";
    }
}

void on_move(ui::Move direction)
{
    const int motion = direction == ui::Move::Up ? 1 : direction == ui::Move::Down ? -1 : 0;
    if (motion != 0) {
        clear_active();
    }
    s_hold.store(motion, std::memory_order_relaxed);
    if (over_ble()) {
        if (motion == 0) {
            ble::desk::stop();  // even a hand that was never holding can end a travel
            return;
        }
        ble::desk::hold(deskproto::motion_of(motion));
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(motion > 0   ? loctek::Move::Up
                                                           : motion < 0 ? loctek::Move::Down
                                                                        : loctek::Move::Stop));
    }
}

void on_network_move(ui::Move direction)
{
    if (!linked()) {
        ESP_LOGW(TAG, "ignoring a move from the network: the desk is not linked");
        return;
    }
    s_network_hold_until.store(
        direction == ui::Move::Stop ? 0 : xTaskGetTickCount() + NETWORK_HOLD,
        std::memory_order_relaxed);
    on_move(direction);
}

}  // namespace desk
