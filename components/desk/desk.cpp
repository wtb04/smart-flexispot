#include "desk.h"

#include "deskproto.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "loctek.h"
#include "settings.h"
#include "transport.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace desk {
namespace detail {
namespace {
struct TraceLine {
    std::uint32_t ms;
    const char   *what;
    int           a;
    int           b;
};
constexpr int TRACE_LINES = 400;
TraceLine    *s_trace = nullptr;  // in PSRAM, made on the first line: internal RAM is short
int           s_trace_next  = 0;
int           s_trace_count = 0;
portMUX_TYPE  s_trace_lock  = portMUX_INITIALIZER_UNLOCKED;
}  // namespace

void trace(const char *what, int a, int b)
{
    const auto ms = static_cast<std::uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_trace == nullptr) {
        auto *lines = static_cast<TraceLine *>(heap_caps_calloc(TRACE_LINES, sizeof(TraceLine), MALLOC_CAP_SPIRAM));
        if (lines == nullptr) {
            return;
        }
        portENTER_CRITICAL(&s_trace_lock);
        if (s_trace == nullptr) {
            s_trace = lines;
            lines   = nullptr;
        }
        portEXIT_CRITICAL(&s_trace_lock);
        heap_caps_free(lines);  // another task made it first
    }
    portENTER_CRITICAL(&s_trace_lock);
    s_trace[s_trace_next] = {ms, what, a, b};
    s_trace_next          = (s_trace_next + 1) % TRACE_LINES;
    s_trace_count         = std::min(s_trace_count + 1, TRACE_LINES);
    portEXIT_CRITICAL(&s_trace_lock);
}
}  // namespace detail

int trace_text(char *out, std::size_t size, bool clear)
{
    static auto *copy = static_cast<detail::TraceLine *>(
        heap_caps_calloc(detail::TRACE_LINES, sizeof(detail::TraceLine), MALLOC_CAP_SPIRAM));
    if (copy == nullptr || detail::s_trace == nullptr) {
        return 0;
    }
    portENTER_CRITICAL(&detail::s_trace_lock);
    const int count = detail::s_trace_count;
    const int first = (detail::s_trace_next - count + detail::TRACE_LINES) % detail::TRACE_LINES;
    for (int i = 0; i < count; ++i) {
        copy[i] = detail::s_trace[(first + i) % detail::TRACE_LINES];
    }
    if (clear) {
        detail::s_trace_count = 0;
    }
    portEXIT_CRITICAL(&detail::s_trace_lock);
    int n = 0;
    for (int i = 0; i < count && n < static_cast<int>(size) - 1; ++i) {
        n += std::snprintf(out + n, size - n, "%8u %-14s %6d %4d\n", static_cast<unsigned>(copy[i].ms), copy[i].what,
                           copy[i].a, copy[i].b);
    }
    return std::min(n, static_cast<int>(size) - 1);
}

namespace {
constexpr char TAG[] = "desk";

using detail::kAsleep;
using detail::kConnected;

detail::Transport *s_link = nullptr;

View s_view{};

void show_preset_active(int index, bool active)
{
    if (s_view.preset_active != nullptr) {
        s_view.preset_active(index, active);
    }
}

void show_height(int height_mm)
{
    if (s_view.height != nullptr) {
        s_view.height(height_mm);
    }
}

void show_available(bool linked)
{
    if (s_view.available != nullptr) {
        s_view.available(linked);
    }
}

void show_notice(const char *message, Tone tone, int timeout_ms)
{
    if (s_view.notice != nullptr) {
        s_view.notice(message, tone, timeout_ms);
    }
}

constexpr TickType_t SUPERVISE_TICK = pdMS_TO_TICKS(200);
constexpr TickType_t WAKE_RETRY     = pdMS_TO_TICKS(5000);

constexpr std::uint32_t TASK_STACK    = 4096;  // measured: uses 1.8 KB
constexpr std::uint32_t PUMP_STACK    = 2048;  // measured: uses 0.7 KB
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

constexpr int        DEPARTED_MM   = 8;
constexpr TickType_t STILL_TIME    = pdMS_TO_TICKS(350);
constexpr TickType_t COMMAND_GRACE = pdMS_TO_TICKS(4000);

constexpr int SAVED_NOTICE_MS = 2500;
constexpr int HINT_NOTICE_MS  = 3000;

// Home Assistant's move buttons send one message and no release.
constexpr TickType_t NETWORK_HOLD = pdMS_TO_TICKS(1500);
std::atomic<TickType_t> s_network_hold_until{0};  // zero when no such hold is live

int  s_preset_mm[deskproto::kPresetCount] = {-1, -1, -1, -1, -1, -1};
bool s_presets_dirty                      = false;
std::atomic<int> s_active_preset{-1};

using Presets = std::array<int, deskproto::kPresetCount>;

// A record from before presets 5 and 6 holds four; those are kept.
bool accept_presets(const void *bytes, std::size_t size, Presets &out)
{
    if (size % sizeof(int) != 0 || size > sizeof(Presets)) {
        return false;
    }
    out.fill(-1);
    std::memcpy(out.data(), bytes, size);
    return true;
}

settings::Record<Presets> s_stored_presets{NVS_NAMESPACE, NVS_PRESETS, [] {
                                               Presets none;
                                               none.fill(-1);
                                               return none;
                                           }(),
                                           accept_presets};

void load_presets()
{
    const Presets stored = s_stored_presets.get();
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        s_preset_mm[i] = loctek::in_range(stored[i]) ? stored[i] : -1;
    }
    ESP_LOGI(TAG, "presets %d, %d, %d, %d, %d, %d mm", s_preset_mm[0], s_preset_mm[1], s_preset_mm[2],
             s_preset_mm[3], s_preset_mm[4], s_preset_mm[5]);
}

void save_presets()
{
    Presets heights;
    std::copy(std::begin(s_preset_mm), std::end(s_preset_mm), heights.begin());
    s_stored_presets.set(heights);
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
        show_preset_active(i, active);
    }
    s_active_preset.store(standing_at, std::memory_order_relaxed);
}

void clear_active()
{
    s_active_preset.store(-1, std::memory_order_relaxed);
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        show_preset_active(i, false);
    }
}

std::atomic<int> s_height_mm{-1};

TickType_t s_commanded_at   = 0;  // zero when nothing is expected to move
int        s_commanded_from = -1;

std::atomic<bool> s_linked{false};

// -1 down, 0 idle, +1 up: what this panel is holding. Over Bluetooth the
// link's own report is a round trip behind, so the hold answers first.
std::atomic<int> s_hold{0};

int motion_now()
{
    const int hold = s_hold.load(std::memory_order_relaxed);
    return hold != 0 || s_link == nullptr ? hold : s_link->motion();
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
    return s_link->goto_height(height_mm);
}

void let_go()
{
    s_hold.store(0, std::memory_order_relaxed);
    s_link->move(0);
}

void run_preset(const PresetCommand &cmd)
{
    detail::trace(cmd.store ? "asked store" : "asked preset", cmd.index + 1);
    if (cmd.index < 0 || cmd.index >= deskproto::kPresetCount) {
        return;
    }
    const int height = s_height_mm.load(std::memory_order_relaxed);
    if (cmd.store) {
        if (cmd.index < deskproto::kBoxPresets) {
            s_link->store(cmd.index);
        }
        remember_preset(cmd.index, height);
        show_notice("Preset saved", Tone::Done, SAVED_NOTICE_MS);
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    if (cmd.index >= deskproto::kBoxPresets) {
        if (s_preset_mm[cmd.index] < 0) {
            show_notice("Hold it to save the height it goes to", Tone::Hint, HINT_NOTICE_MS);
            return;
        }
        if (s_link->travelling() && s_travel_index == cmd.index) {
            let_go();  // tapping it again is how a travel is cancelled, as with the box's own
            return;
        }
        if (!travel_to(s_preset_mm[cmd.index])) {
            return;
        }
        s_travel_index = cmd.index;
    } else {
        s_link->preset(cmd.index);
    }
    clear_active();
    s_commanded_from = height;
    s_commanded_at   = now;
}

void on_height(int height_mm)
{
    if (s_height_mm.exchange(height_mm, std::memory_order_relaxed) != height_mm) {
        show_height(height_mm);
    }
}

std::atomic<int> s_pending_height{-1};
TaskHandle_t     s_height_pump = nullptr;

StaticTask_t s_pump_ctrl;
StackType_t  s_pump_stack[PUMP_STACK];

// Heights arrive on the radio host task or the UART receive task; neither may
// wait on the screen, so the pump carries them over.
void on_link_height(int height_mm)
{
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

// What the supervisor carries from one tick to the next.
struct Watch {
    const char *shown         = nullptr;
    TickType_t  last_wake     = 0;
    int         wake_attempts = 0;
    int         settled_at    = -1;
    TickType_t  settled_since = 0;
};

void note_settling(Watch &watch, int height, TickType_t now)
{
    if (height != watch.settled_at) {
        watch.settled_at    = height;
        watch.settled_since = now;
    }
}

void save_presets_if_changed()
{
    if (s_presets_dirty) {
        s_presets_dirty = false;
        save_presets();
    }
}

void let_go_of_stale_network_hold(TickType_t now)
{
    const TickType_t network_until = s_network_hold_until.load(std::memory_order_relaxed);
    if (network_until != 0 && static_cast<std::int32_t>(now - network_until) >= 0) {
        s_network_hold_until.store(0, std::memory_order_relaxed);
        ESP_LOGW(TAG, "a move from the network was not asked for again, letting go");
        on_move(Move::Stop);
    }
}

/** Whether a command was still expected to move the desk as this tick began.
 *  It stops being once the desk has left, or after the grace period. */
bool command_in_flight(int height, TickType_t now)
{
    const bool commanded = s_commanded_at != 0;
    if (commanded) {
        const bool left = height >= 0 && s_commanded_from >= 0 &&
                          std::abs(height - s_commanded_from) > DEPARTED_MM;
        if (left || now - s_commanded_at > COMMAND_GRACE) {
            s_commanded_at = 0;
        }
    }
    return commanded;
}

void show_status(Watch &watch, const char *status)
{
    if (status == watch.shown) {
        return;
    }
    watch.shown       = status;
    const bool linked = status == kConnected;
    s_linked.store(linked, std::memory_order_relaxed);
    show_available(linked);
    show_height(linked ? s_height_mm.load(std::memory_order_relaxed) : -1);
    ESP_LOGI(TAG, "%s", status);
    if (status == kAsleep) {
        s_link->nudge();
    }
}

void wake_if_never_heard(Watch &watch, const char *status, TickType_t now)
{
    const bool never_read = s_height_mm.load(std::memory_order_relaxed) < 0;
    if (never_read && s_link->can_wake() && status != kConnected &&
        (watch.last_wake == 0 || now - watch.last_wake > WAKE_RETRY)) {
        watch.last_wake = now;
        ++watch.wake_attempts;
        s_link->wake();
    }
}

[[noreturn]] void supervisor_task(void *)
{
    Watch watch;

    for (;;) {
        PresetCommand cmd;
        if (xQueueReceive(s_preset_queue, &cmd, SUPERVISE_TICK) == pdTRUE) {
            run_preset(cmd);
        }

        const TickType_t now     = xTaskGetTickCount();
        const int        height  = s_height_mm.load(std::memory_order_relaxed);
        const char      *status  = s_link->status(now, height >= 0, watch.wake_attempts);
        const bool       link_up = status == kConnected;

        note_settling(watch, height, now);
        save_presets_if_changed();
        let_go_of_stale_network_hold(now);
        const bool moving = command_in_flight(height, now) || motion_now() != 0 ||
                            s_link->travelling() || now - watch.settled_since < STILL_TIME;
        publish_active(height, link_up, moving);

        show_status(watch, status);
        wake_if_never_heard(watch, status, now);
    }
}

}  // namespace

esp_err_t start(Link link, const View &view)
{
    s_view = view;
    QueueHandle_t presets =
        xQueueCreateStatic(PRESET_QUEUE_LEN, sizeof(PresetCommand),
                           reinterpret_cast<std::uint8_t *>(s_preset_queue_storage),
                           &s_preset_queue_ctrl);
    ESP_RETURN_ON_FALSE(presets != nullptr, ESP_ERR_NO_MEM, TAG, "preset queue");
    s_preset_queue = presets;

    load_presets();

    s_link = link == Link::Bluetooth ? &detail::bluetooth() : &detail::wire();
    ESP_LOGI(TAG, "driving the desk over %s", s_link->name());
    s_height_pump = xTaskCreateStatic(height_pump_task, "deskht", PUMP_STACK, nullptr,
                                      TASK_PRIORITY, s_pump_stack, &s_pump_ctrl);
    ESP_RETURN_ON_FALSE(s_height_pump != nullptr, ESP_ERR_NO_MEM, TAG, "height pump");
    ESP_RETURN_ON_ERROR(s_link->start(on_link_height), TAG, "link");

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

void on_move(Move direction)
{
    detail::trace("asked move", static_cast<int>(direction));
    if (s_link == nullptr) {
        return;  // a touch before the desk has started
    }
    const int motion = static_cast<int>(direction);
    if (motion != 0) {
        clear_active();
    }
    s_hold.store(motion, std::memory_order_relaxed);
    s_link->move(motion);
}

void on_network_move(Move direction)
{
    if (!linked()) {
        ESP_LOGW(TAG, "ignoring a move from the network: the desk is not linked");
        return;
    }
    s_network_hold_until.store(
        direction == Move::Stop ? 0 : xTaskGetTickCount() + NETWORK_HOLD,
        std::memory_order_relaxed);
    on_move(direction);
}

}  // namespace desk
