#include "desk.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "loctek.h"

#include <atomic>

#include "freertos/queue.h"

namespace desk {
namespace {

constexpr char TAG[] = "desk";
constexpr char kConnected[] = "connected";
constexpr char kAsleep[]    = "desk display asleep";
constexpr char kNoReply[]   = "disconnected";
constexpr char kWaking[]    = "waking desk";

// A sleeping control box is silent, so at startup "nothing on the wire" is the
// normal state rather than a fault. Only call it a wiring problem once we have
// pulsed the wake line a few times and still heard nothing.
constexpr int WIRING_FAULT_AFTER_WAKES = 3;

constexpr TickType_t SUPERVISE_TICK = pdMS_TO_TICKS(500);
// The box streams in bursts with gaps of a second or so between them, and the
// wake line is held high so it should not be sleeping at all. Only a longer
// silence than that means the link has actually dropped.
constexpr TickType_t LINK_TIMEOUT = pdMS_TO_TICKS(3000);
// How often to re-pulse the wake line while waiting for the first reading. One
// pulse can be missed; without a retry the height never appears at all.
constexpr TickType_t WAKE_RETRY = pdMS_TO_TICKS(5000);

constexpr std::uint32_t TASK_STACK    = 3072;
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

// Preset work cannot run in the LVGL callback: storing one blocks for the gap
// between the M key and the preset key. The supervisor task does it instead.
struct PresetCommand {
    int  index;
    bool store;
};

constexpr UBaseType_t PRESET_QUEUE_LEN = 4;
StaticQueue_t s_preset_queue_ctrl;
PresetCommand s_preset_queue_storage[PRESET_QUEUE_LEN];
QueueHandle_t s_preset_queue = nullptr;

void run_preset(const PresetCommand &cmd)
{
    if (cmd.index < 0 || cmd.index >= ui::kPresetCount) {
        return;
    }
    const auto preset = static_cast<loctek::Preset>(cmd.index);
    if (cmd.store) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::store_preset(preset));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::notify("", "Preset saved", "success", 2500));
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::goto_preset(preset));
    }
}

std::atomic<int>  s_height_mm{-1};
std::atomic<bool> s_linked{false};
std::atomic<int>  s_motion{0};  // -1 down, 0 idle, +1 up

// Straight from the wire. Repaints only when the value actually changes: the
// box streams frames far faster than the display needs, and redrawing a label
// that already reads correctly just steals time from the LVGL task.
void on_height(int height_mm)
{
    if (s_height_mm.exchange(height_mm, std::memory_order_relaxed) != height_mm) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_height(height_mm));
    }
}

// The wake line is held high, so the box should stream continuously. Anything
// else is a fault worth naming precisely: nothing on the wire, bytes that never
// form frames, or a link that has gone quiet.
const char *link_status(const loctek::Stats &stats, bool link_up, int wake_attempts)
{
    if (link_up) {
        // Height frames with nothing readable in them mean the box has blanked
        // its display. Only interesting before the first reading: afterwards
        // the last height stands.
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

// Reporting the state of the link is most of the battle when wiring a control
// box up for the first time.
[[noreturn]] void supervisor_task(void *)
{
    const char   *shown      = nullptr;
    loctek::Stats previous   = loctek::stats();
    TickType_t    last_wake     = 0;
    TickType_t    last_frame    = 0;
    int           wake_attempts = 0;

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
        const char *status  = link_status(stats, link_up, wake_attempts);

        if (status != shown) {
            shown = status;
            s_linked.store(status == kConnected, std::memory_order_relaxed);
            ESP_LOGI(TAG, "%s", status);
#if CONFIG_LOCTEK_NUDGE_WAKE
            if (status == kAsleep) {
                // Only ever reached before the first reading, so the desk takes
                // one step at startup and none after. Nudging on every sleep
                // cycle would creep the desk upward all day.
                ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::nudge());
            }
#endif

            // The last known height stays on screen; only the status changes.
        }
        // Only until the first reading lands. After that the last known height
        // stands and the box is left to sleep; loctek wakes it again by itself
        // when a move is asked for. A fully asleep box transmits nothing at
        // all, so this must not be gated on hearing anything from it.
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(loctek::request_move(move));
}

}  // namespace desk
