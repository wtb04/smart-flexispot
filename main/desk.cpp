#include "desk.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loctek.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <atomic>
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

// A sleeping control box is silent, so "nothing on the wire" is normal at
// startup. Only a wiring fault once the wake line has been pulsed this often.
constexpr int WIRING_FAULT_AFTER_WAKES = 3;

constexpr TickType_t SUPERVISE_TICK = pdMS_TO_TICKS(200);
// The box streams in bursts with gaps of a second or so between them, so only a
// longer silence than that means the link has dropped.
constexpr TickType_t LINK_TIMEOUT = pdMS_TO_TICKS(3000);
// One wake pulse can be missed, and without a retry the height never appears.
constexpr TickType_t WAKE_RETRY = pdMS_TO_TICKS(5000);

// This task puts notifications on screen: it copies a Notice onto its stack and
// then walks LVGL's label and layout paths.
constexpr std::uint32_t TASK_STACK    = 6144;
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

// Preset work cannot run in the LVGL callback: storing one blocks for the gap
// between the M key and the preset key, so the supervisor task does it.
struct PresetCommand {
    int  index;
    bool store;
};

constexpr UBaseType_t PRESET_QUEUE_LEN = 4;
StaticQueue_t s_preset_queue_ctrl;
PresetCommand s_preset_queue_storage[PRESET_QUEUE_LEN];
QueueHandle_t s_preset_queue = nullptr;

// The control box never reports what a preset is set to, so the panel learns:
// a height stored to a preset is that preset's height, and so is wherever the
// desk comes to rest after being sent to one. Kept in NVS because a desk that
// has not moved since the last boot is still standing at a preset.
constexpr char NVS_NAMESPACE[] = "desk";
// Bumped when the learning changes meaning: the first version recorded the
// height a preset was pressed *from*, so anything stored under it is wrong.
constexpr char NVS_PRESETS[]   = "presets2";

// The box stops within a few millimetres of where it was asked to, and the
// readout moves in whole millimetres, so this is about matching intent.
constexpr int PRESET_TOLERANCE_MM = 8;

// Long enough to be sure the desk has finished, because recording the wrong
// height teaches the preset something wrong and it persists.
constexpr TickType_t SETTLE_TIME = pdMS_TO_TICKS(1500);
// Short, because this only decides whether to light a button: the box reports
// a new height several times a second while travelling, so a brief quiet
// period already means it has stopped.
constexpr TickType_t STILL_TIME = pdMS_TO_TICKS(350);

// A preset takes a moment to get the desk going, and for that moment the height
// is exactly where it has been sitting -- which read as still standing at the
// preset that was lit, so the highlight came back on and went off again as the
// desk finally moved. Motion is assumed from the command until the height
// proves it, or until this runs out and the desk evidently is not going
// anywhere, which is the case when it was already there.
constexpr TickType_t COMMAND_GRACE = pdMS_TO_TICKS(4000);

constexpr TickType_t LEARN_TIMEOUT = pdMS_TO_TICKS(45000);

int  s_preset_mm[ui::kPresetCount] = {-1, -1, -1, -1};
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
        default: return "none";
    }
}

void load_presets()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    std::size_t size = sizeof(s_preset_mm);
    if (nvs_get_blob(handle, NVS_PRESETS, s_preset_mm, &size) != ESP_OK ||
        size != sizeof(s_preset_mm)) {
        for (int &mm : s_preset_mm) {
            mm = -1;
        }
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

void publish_active(int height_mm, bool linked, bool moving)
{
    int standing_at = -1;
    for (int i = 0; i < ui::kPresetCount; ++i) {
        const bool active = linked && !moving && height_mm >= 0 && s_preset_mm[i] >= 0 &&
                            std::abs(height_mm - s_preset_mm[i]) <= PRESET_TOLERANCE_MM;
        if (active) {
            standing_at = i;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_preset_active(i, active));
    }
    s_active_preset.store(standing_at, std::memory_order_relaxed);
}

// The moment the desk is asked to move, standing at a preset stops being true.
// Waiting for the supervisor's next tick, or for the height to leave the
// tolerance, would leave the highlight up while the desk was already moving.
void clear_active()
{
    s_active_preset.store(-1, std::memory_order_relaxed);
    for (int i = 0; i < ui::kPresetCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_preset_active(i, false));
    }
}

std::atomic<int>  s_height_mm{-1};

TickType_t s_commanded_at   = 0;  // zero when nothing is expected to move
int        s_commanded_from = -1;

void run_preset(const PresetCommand &cmd)
{
    if (cmd.index < 0 || cmd.index >= ui::kPresetCount) {
        return;
    }
    const auto preset = static_cast<loctek::Preset>(cmd.index);
    if (cmd.store) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::store_preset(preset));
        remember_preset(cmd.index, s_height_mm.load(std::memory_order_relaxed));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::notify("", "Preset saved", "success", 2500));
    } else {
        clear_active();
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::goto_preset(preset));
        s_learning        = cmd.index;
        s_learn_from      = s_height_mm.load(std::memory_order_relaxed);
        s_learn_started   = xTaskGetTickCount();
        s_commanded_from  = s_learn_from;
        s_commanded_at    = s_learn_started;
    }
}

std::atomic<bool> s_linked{false};
std::atomic<int>  s_motion{0};  // -1 down, 0 idle, +1 up

// Repaints only on a change: the box streams frames far faster than the display
// needs, and redrawing an already-correct label just costs the LVGL task time.
void on_height(int height_mm)
{
    if (s_height_mm.exchange(height_mm, std::memory_order_relaxed) != height_mm) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_height(height_mm));
    }
}

const char *link_status(const loctek::Stats &stats, bool link_up, int wake_attempts)
{
    if (link_up) {
        // Height frames with nothing readable in them mean the box has blanked
        // its display.
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

        const bool  link_up = last_frame != 0 && (now - last_frame) < LINK_TIMEOUT;

        const int height = s_height_mm.load(std::memory_order_relaxed);
        if (height != settled_at) {
            settled_at    = height;
            settled_since = now;
        }
        if (s_learning >= 0) {
            // The desk has not started moving yet when the preset is pressed,
            // so the height is already long settled. Recording it then taught
            // the preset whatever height it was sent from -- which is how SIT
            // came to be stored as STAND's height. Wait for it to actually
            // move, and give up if it never does.
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
        // Tied to the height going quiet rather than to the learning, which
        // waits far longer on purpose and was holding the highlight back for
        // a second and a half after the desk had visibly stopped.
        const bool commanded = s_commanded_at != 0;
        if (commanded) {
            const bool left = height >= 0 && s_commanded_from >= 0 &&
                              std::abs(height - s_commanded_from) > PRESET_TOLERANCE_MM;
            if (left || now - s_commanded_at > COMMAND_GRACE) {
                s_commanded_at = 0;
            }
        }
        const bool moving = commanded || s_motion.load(std::memory_order_relaxed) != 0 ||
                            now - settled_since < STILL_TIME;
        publish_active(height, link_up, moving);

        const char *status  = link_status(stats, link_up, wake_attempts);

        if (status != shown) {
            shown = status;
            const bool linked = status == kConnected;
            s_linked.store(linked, std::memory_order_relaxed);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_desk_available(linked));
            // Pushed again on every transition: on_height only fires on a
            // change, so a dropout would leave dashes until the desk next moved.
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                ui::set_height(linked ? s_height_mm.load(std::memory_order_relaxed) : -1));
            ESP_LOGI(TAG, "%s", status);
#if CONFIG_LOCTEK_NUDGE_WAKE
            if (status == kAsleep) {
                // Only reached before the first reading: nudging on every sleep
                // cycle would creep the desk upward all day.
                ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::nudge());
            }
#endif
        }
        // Only until the first reading lands; after that the box is left to
        // sleep. A fully asleep box transmits nothing, so this must not be gated
        // on hearing anything from it.
        const bool never_read = s_height_mm.load(std::memory_order_relaxed) < 0;
        if (never_read && status != kConnected && (last_wake == 0 || now - last_wake > WAKE_RETRY)) {
            last_wake = now;
            ++wake_attempts;
            ESP_LOGI(TAG, "waking panel");
            ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::wake());
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
    ESP_RETURN_ON_ERROR(loctek::start(on_height), TAG, "loctek");

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
    ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(move));
}

}  // namespace desk
