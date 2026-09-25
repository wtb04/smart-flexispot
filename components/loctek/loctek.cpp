#include "loctek.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "nvs.h"
#include "soc/soc_caps.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace loctek {
namespace {
constexpr char TAG[] = "loctek";

constexpr auto        UART            = static_cast<uart_port_t>(CONFIG_LOCTEK_UART_NUM);
constexpr int         BAUD_RATE       = 9600;
constexpr int         RX_BUF_SIZE     = 1024;
constexpr int         TX_BUF_SIZE     = 0;
constexpr std::size_t RX_CHUNK        = 64;
constexpr TickType_t  RX_READ_TIMEOUT = pdMS_TO_TICKS(10);
constexpr TickType_t  TX_DONE_TIMEOUT = pdMS_TO_TICKS(100);

constexpr TickType_t REPEAT_TICKS    = pdMS_TO_TICKS(CONFIG_LOCTEK_REPEAT_MS);
constexpr TickType_t IDLE_POLL_TICKS = pdMS_TO_TICKS(CONFIG_LOCTEK_IDLE_POLL_MS);
constexpr TickType_t MOVE_TIMEOUT    = pdMS_TO_TICKS(CONFIG_LOCTEK_MOVE_TIMEOUT_MS);
constexpr TickType_t WAKE_LOW        = pdMS_TO_TICKS(200);
constexpr int        STOP_REPEATS    = 3;

constexpr int MEMORY_HOLD_LIMIT_MS = 2000;
static_assert(CONFIG_LOCTEK_PRESS_MS < MEMORY_HOLD_LIMIT_MS,
              "M must not be held near the factory-reset time");

// A key whose height stops changing has reached the end of the range, or lost
// the box, long before the travel timeout would notice.
constexpr TickType_t STALL = pdMS_TO_TICKS(2500);

// A travel whose height moves away from its target is fighting something, and
// gives up rather than wrestle with it.
constexpr int RETREAT_MM = 15;

// A travel steers only on a height the box has said recently. Its display goes
// dark after about ten seconds idle and then it reports nothing, so a travel
// asked for then wakes it first and waits this long for it to speak.
constexpr TickType_t HEIGHT_FRESH = pdMS_TO_TICKS(2000);
constexpr TickType_t HEIGHT_WAIT  = pdMS_TO_TICKS(5000);
// The wake pulse does not always light the display; a real key always does, so
// one step toward the target follows if the pulse was not enough.
constexpr TickType_t NUDGE_AFTER  = pdMS_TO_TICKS(1500);

// The desk runs on after the last key frame. How far is learned from where each
// travel comes to rest, per direction, and kept across reboots.
constexpr int        DIRECTIONS      = 2;
constexpr int        RUN_ON_MAX_MM   = 40;
constexpr TickType_t SETTLED_GAP     = pdMS_TO_TICKS(400);
constexpr char       NVS_NAMESPACE[] = "loctek";
constexpr char       NVS_RUN_ON[]    = "runon";

// Below a metre the display shows tenths of a centimetre, which is millimetres;
// above, whole centimetres.
constexpr int FINE_READING_BELOW_MM = 1000;

constexpr std::uint32_t DRIVER_STACK  = 3072;
constexpr std::uint32_t RX_STACK      = 3072;
constexpr UBaseType_t   TASK_PRIORITY = 6;
constexpr BaseType_t    TASK_CORE     = 0;

constexpr KeyFrame FRAME_STOP = build_key_frame(Key::None);
constexpr KeyFrame FRAME_UP   = build_key_frame(Key::Up);
constexpr KeyFrame FRAME_DOWN = build_key_frame(Key::Down);

// A key press with a duration, done in order: the box wants a stream of frames
// and a release for each.
struct Press {
    enum class Kind : std::uint8_t { Key, Store, Wake, Nudge };
    Kind kind;
    Key  key;
};
constexpr UBaseType_t PRESS_QUEUE_LEN = 4;
constexpr UBaseType_t MAILBOX_DEPTH   = 1;

// Two mailboxes and a queue feed the driver. Each mailbox holds the latest
// wish of its kind, a newer one replacing the old, so a Stop can never queue
// behind anything and a hand's move is never lost to a travel request.
StaticQueue_t s_move_ctrl;
Move          s_move_storage[MAILBOX_DEPTH];
QueueHandle_t s_moves = nullptr;

StaticQueue_t s_goto_ctrl;
int           s_goto_storage[MAILBOX_DEPTH];
QueueHandle_t s_gotos = nullptr;

StaticQueue_t s_press_ctrl;
Press         s_press_storage[PRESS_QUEUE_LEN];
QueueHandle_t s_presses = nullptr;

std::atomic<bool> s_started{false};  // set last, once the tasks are running

TaskHandle_t s_driver = nullptr;

StaticTask_t s_driver_ctrl;
StaticTask_t s_rx_ctrl;
StackType_t *s_driver_stack = nullptr;
StackType_t *s_rx_stack     = nullptr;

HeightHandler s_on_height = nullptr;

std::atomic<std::uint32_t> s_bytes_received{0};
std::atomic<std::uint32_t> s_frames_decoded{0};
std::atomic<std::uint32_t> s_height_frames{0};
std::atomic<std::uint32_t> s_heights_decoded{0};

// Published by the receive task.
std::atomic<int>        s_last_height{-1};
std::atomic<TickType_t> s_last_height_at{0};
std::atomic<bool>       s_height_posted{false};

// Published by the driver task, and by request_move for the hand.
std::atomic<std::int8_t> s_motion{0};
std::atomic<int>         s_travelling_to{-1};
std::atomic<bool>        s_hand{false};

void wake_driver()
{
    if (s_driver != nullptr) {
        xTaskNotifyGive(s_driver);
    }
}

bool reached(TickType_t now, TickType_t when)
{
    return static_cast<std::int32_t>(now - when) >= 0;
}

// ---- Driver task state. Touched on the driver task only. ----

Move       d_direction   = Move::Stop;
TickType_t d_deadline    = 0;
TickType_t d_progress_at = 0;  // when the height last changed, or a key went down
TickType_t d_next_frame  = 0;
int        d_seen_height = -1;

int        d_target    = -1;
int        d_best_away = INT_MAX;  // the closest this travel has come
TickType_t d_asked_at  = 0;        // when the travel was asked for
bool       d_nudged    = false;
int        d_run_on[DIRECTIONS] = {};

int        d_landing_target = -1;
Move       d_landing_dir    = Move::Stop;
int        d_landing_last   = -1;
TickType_t d_landing_at     = 0;

bool moving()
{
    return d_direction != Move::Stop;
}

bool travelling()
{
    return d_target >= 0;
}

bool awaiting_height()
{
    return travelling() && !moving();
}

int dir_index(Move direction)
{
    return direction == Move::Down ? 1 : 0;
}

const char *dir_name(Move direction)
{
    return direction == Move::Down ? "down" : "up";
}

const KeyFrame &frame_for(Move direction)
{
    switch (direction) {
        case Move::Up:   return FRAME_UP;
        case Move::Down: return FRAME_DOWN;
        case Move::Stop: break;
    }
    return FRAME_STOP;
}

esp_err_t write_frame(const KeyFrame &frame)
{
    const int written = uart_write_bytes(UART, frame.data(), frame.size());
    if (written != static_cast<int>(frame.size())) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(UART, TX_DONE_TIMEOUT);
}

void load_run_on()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    int         stored[DIRECTIONS];
    std::size_t size = sizeof(stored);
    if (nvs_get_blob(handle, NVS_RUN_ON, stored, &size) == ESP_OK && size == sizeof(stored)) {
        for (int i = 0; i < DIRECTIONS; ++i) {
            d_run_on[i] = std::clamp(stored[i], 0, RUN_ON_MAX_MM);
        }
    }
    nvs_close(handle);
}

void save_run_on()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(handle, NVS_RUN_ON, d_run_on, sizeof(d_run_on)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

/** Lets go of the keys: the release frame, repeated, since one can be missed. */
void release()
{
    if (moving()) {
        d_direction = Move::Stop;
        s_motion.store(0, std::memory_order_relaxed);
    }
    for (int i = 0; i < STOP_REPEATS; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(FRAME_STOP));
    }
    d_next_frame = xTaskGetTickCount() + IDLE_POLL_TICKS;
}

void end_travel(const char *why)
{
    d_landing_target = -1;
    if (!travelling()) {
        return;
    }
    ESP_LOGI(TAG, "travel to %d mm ended: %s", d_target, why);
    d_target = -1;
    s_travelling_to.store(-1, std::memory_order_relaxed);
}

void set_direction(Move direction, TickType_t now)
{
    if (direction == Move::Stop) {
        release();
        return;
    }
    if (direction == d_direction) {
        return;
    }
    d_direction   = direction;
    d_deadline    = now + MOVE_TIMEOUT;
    d_progress_at = now;
    d_next_frame  = now;
    s_motion.store(static_cast<std::int8_t>(direction), std::memory_order_relaxed);
}

void watch_landing(int target_mm, Move direction)
{
    d_landing_target = target_mm;
    d_landing_dir    = direction;
    d_landing_last   = -1;
    d_landing_at     = 0;
}

void steer_travel(int height_mm, TickType_t now)
{
    const int away = std::abs(d_target - height_mm);
    if (away < d_best_away) {
        d_best_away = away;
    } else if (moving() && away - d_best_away > RETREAT_MM) {
        ESP_LOGW(TAG, "desk moving away from %d mm, giving up", d_target);
        end_travel("moving away");
        release();
        return;
    }

    const Move heading = moving()                ? d_direction
                         : d_target > height_mm ? Move::Up
                                                : Move::Down;
    const int  margin  = d_run_on[dir_index(heading)];
    const Move next    = steer(d_target, height_mm, d_direction, margin);
    if (next == Move::Stop) {
        const Move was    = d_direction;
        const int  target = d_target;
        ESP_LOGI(TAG, "releasing at %d mm for %d mm", height_mm, target);
        end_travel("arrived");
        release();
        // A new target releases the old travel before it starts, so a release
        // here while moving is always this travel arriving, and worth learning
        // from whether it stopped inside the margin or had already crossed.
        if (was != Move::Stop) {
            watch_landing(target, was);
        }
        return;
    }
    if (next != d_direction) {
        set_direction(next, now);
    }
}

void land(int height_mm, TickType_t now)
{
    if (height_mm != d_landing_last) {
        d_landing_last = height_mm;
        d_landing_at   = now;
        return;
    }
    if (now - d_landing_at < SETTLED_GAP) {
        return;
    }
    const int target = d_landing_target;
    d_landing_target = -1;

    const int past = d_landing_dir == Move::Up ? height_mm - target : target - height_mm;
    if (past == 0) {
        ESP_LOGI(TAG, "landed on %d mm", target);
        return;
    }
    // Where the box reports millimetres the whole miss is taken and the next
    // trip lands on it. Where it reports whole centimetres, taking all of one
    // would overshoot the other way, so half at a time.
    int       &run_on = d_run_on[dir_index(d_landing_dir)];
    const int  was    = run_on;
    const bool fine   = target < FINE_READING_BELOW_MM && height_mm < FINE_READING_BELOW_MM;
    const int  step   = fine ? past : (past + (past > 0 ? 1 : -1)) / 2;
    run_on            = std::clamp(run_on + step, 0, RUN_ON_MAX_MM);
    ESP_LOGI(TAG, "landed %d mm %s %d mm; run-on %s is now %d mm", std::abs(past),
             past > 0 ? "past" : "short of", target, dir_name(d_landing_dir), run_on);
    if (run_on != was) {
        save_run_on();
    }
}

void on_height(int height_mm, TickType_t now)
{
    if (height_mm != d_seen_height) {
        d_seen_height = height_mm;
        d_progress_at = now;
    }
    if (travelling()) {
        steer_travel(height_mm, now);
    } else if (d_landing_target >= 0) {
        land(height_mm, now);
    }
}

esp_err_t turn_on()
{
    if constexpr (CONFIG_LOCTEK_WAKE_GPIO < 0) {
        return ESP_OK;
    } else {
        constexpr auto pin = static_cast<gpio_num_t>(CONFIG_LOCTEK_WAKE_GPIO);
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 0), TAG, "wake low");
        vTaskDelay(WAKE_LOW);
        ESP_RETURN_ON_ERROR(gpio_set_level(pin, 1), TAG, "wake high");
        vTaskDelay(pdMS_TO_TICKS(CONFIG_LOCTEK_WAKE_PULSE_MS));
        return ESP_OK;
    }
}

void take_move(Move direction, TickType_t now)
{
    end_travel(direction == Move::Stop ? "released" : "a hand on the keys");
    set_direction(direction, now);
}

void take_goto(int target_mm, TickType_t now)
{
    if (s_hand.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "not travelling to %d mm: a hand is on the keys", target_mm);
        return;
    }
    if (travelling() || moving()) {
        end_travel("new target");
        release();
    }
    d_target         = target_mm;
    d_best_away      = INT_MAX;
    d_progress_at    = now;
    d_asked_at       = now;
    d_nudged         = false;
    d_landing_target = -1;
    s_travelling_to.store(target_mm, std::memory_order_relaxed);

    // Every move goes through this driver, so the last height the box said is
    // where the desk still is, even once its display has gone dark. Starting
    // from it lights the display at once, and the first fresh height takes
    // over; a wrong start is caught by the moving-away guard within 15 mm.
    const int here = s_last_height.load(std::memory_order_relaxed);
    if (here >= 0) {
        const bool fresh = now - s_last_height_at.load(std::memory_order_acquire) <= HEIGHT_FRESH;
        ESP_LOGI(TAG, "travelling from %d mm%s to %d mm", here, fresh ? "" : " (last heard)",
                 target_mm);
        steer_travel(here, now);
        return;
    }
    // Never heard at all: steering starts on the first height the woken box reports.
    ESP_LOGI(TAG, "travelling to %d mm, waking the box to hear where it is", target_mm);
    ESP_ERROR_CHECK_WITHOUT_ABORT(turn_on());
    d_next_frame = xTaskGetTickCount();
}

void press_key(Key key, int duration_ms)
{
    const KeyFrame   frame = build_key_frame(key);
    const TickType_t end   = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    esp_err_t        err   = ESP_OK;
    do {
        err = write_frame(frame);
        vTaskDelay(REPEAT_TICKS);
    } while (err == ESP_OK && !reached(xTaskGetTickCount(), end));
    ESP_ERROR_CHECK_WITHOUT_ABORT(err);
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(FRAME_STOP));
}

void take_press(const Press &press)
{
    if (travelling() || moving()) {
        end_travel("key pressed");
        release();
    }
    switch (press.kind) {
        case Press::Kind::Key:
            press_key(press.key, CONFIG_LOCTEK_PRESS_MS);
            break;
        case Press::Kind::Store:
            press_key(Key::Memory, CONFIG_LOCTEK_PRESS_MS);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_LOCTEK_STORE_GAP_MS));
            press_key(press.key, CONFIG_LOCTEK_PRESS_MS);
            break;
        case Press::Kind::Wake:
            ESP_ERROR_CHECK_WITHOUT_ABORT(turn_on());
            ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(FRAME_STOP));
            break;
        case Press::Kind::Nudge:
            ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(FRAME_UP));
            ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(FRAME_STOP));
            break;
    }
    d_next_frame = xTaskGetTickCount() + IDLE_POLL_TICKS;
}

void take_requests(TickType_t now)
{
    Move wanted = Move::Stop;
    if (xQueueReceive(s_moves, &wanted, 0) == pdTRUE) {
        take_move(wanted, now);
    }
    int target = -1;
    if (xQueueReceive(s_gotos, &target, 0) == pdTRUE) {
        take_goto(target, now);
    }
    Press press{};
    while (xQueueReceive(s_presses, &press, 0) == pdTRUE) {
        take_press(press);  // blocks for the press; nothing else writes meanwhile
    }
}

void take_posted_height(TickType_t now)
{
    if (s_height_posted.exchange(false, std::memory_order_acquire)) {
        on_height(s_last_height.load(std::memory_order_relaxed), now);
    }
}

void chase_silent_box(TickType_t now)
{
    if (!awaiting_height()) {
        return;
    }
    if (now - d_asked_at > HEIGHT_WAIT) {
        ESP_LOGW(TAG, "the box did not say where it is, not travelling");
        end_travel("no height");
    } else if (!d_nudged && now - d_asked_at > NUDGE_AFTER) {
        d_nudged        = true;
        const int  last = s_last_height.load(std::memory_order_relaxed);
        const Move step = last >= 0 && d_target < last ? Move::Down : Move::Up;
        ESP_LOGI(TAG, "box still dark, one step %s to wake it", dir_name(step));
        ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(frame_for(step)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(FRAME_STOP));
    }
}

void enforce_limits(TickType_t now)
{
    if (!moving()) {
        return;
    }
    if (reached(now, d_deadline)) {
        ESP_LOGW(TAG, "travel timeout, stopping");
        end_travel("timeout");
        release();
    } else if (now - d_progress_at > STALL) {
        ESP_LOGW(TAG, "height stopped changing, stopping");
        end_travel("stalled");
        release();
    }
}

/** Sends the frame that is due, if one is, and says how long until the next. */
TickType_t poll_box(TickType_t now)
{
    // While waiting to hear the height, ask as often as while moving: the
    // box answers every frame, and each answer is a chance to start.
    const bool eager = moving() || awaiting_height();
    const bool polls = eager || IDLE_POLL_TICKS > 0;
    if (!polls) {
        return portMAX_DELAY;
    }
    if (reached(now, d_next_frame)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(frame_for(d_direction)));
        d_next_frame = now + (eager ? REPEAT_TICKS : IDLE_POLL_TICKS);
    }
    const auto until = static_cast<std::int32_t>(d_next_frame - now);
    return until > 0 ? static_cast<TickType_t>(until) : 0;
}

[[noreturn]] void driver_task(void *)
{
    for (;;) {
        take_requests(xTaskGetTickCount());
        const TickType_t now = xTaskGetTickCount();
        take_posted_height(now);
        chase_silent_box(now);
        enforce_limits(now);
        ulTaskNotifyTake(pdTRUE, poll_box(now));
    }
}

// ---- Receive task. Decodes, publishes, and decides nothing. ----

constexpr int RAW_KEEP = 64;
std::uint8_t  s_raw[RAW_KEEP];
int           s_raw_len = 0;

void keep_raw(const std::uint8_t *bytes, int count)
{
    for (int i = 0; i < count; ++i) {
        if (s_raw_len < RAW_KEEP) {
            s_raw[s_raw_len++] = bytes[i];
        } else {
            std::memmove(s_raw, s_raw + 1, RAW_KEEP - 1);
            s_raw[RAW_KEEP - 1] = bytes[i];
        }
    }
}

void publish_height(int height_mm)
{
    s_heights_decoded.fetch_add(1, std::memory_order_relaxed);
    s_last_height.store(height_mm, std::memory_order_relaxed);
    s_last_height_at.store(xTaskGetTickCount(), std::memory_order_release);
    s_height_posted.store(true, std::memory_order_release);
    wake_driver();
    if (s_on_height != nullptr) {
        s_on_height(height_mm);
    }
}

void take_frame(const Frame &frame)
{
    s_frames_decoded.fetch_add(1, std::memory_order_relaxed);
    if (frame.type() == FrameType::Height) {
        s_height_frames.fetch_add(1, std::memory_order_relaxed);
    }
    if (const std::optional<int> height_mm = decode_height_mm(frame)) {
        publish_height(*height_mm);
    }
}

[[noreturn]] void rx_task(void *)
{
    Parser                             parser;
    std::array<std::uint8_t, RX_CHUNK> buf{};

    for (;;) {
        const int read = uart_read_bytes(UART, buf.data(), buf.size(), RX_READ_TIMEOUT);
        if (read > 0) {
            keep_raw(buf.data(), read);
            s_bytes_received.fetch_add(static_cast<std::uint32_t>(read), std::memory_order_relaxed);
        }
        for (int i = 0; i < read; ++i) {
            if (const std::optional<Frame> frame = parser.push(buf[i])) {
                take_frame(*frame);
            }
        }
    }
}

esp_err_t init_wake_gpio()
{
    if constexpr (CONFIG_LOCTEK_WAKE_GPIO < 0) {
        return ESP_OK;
    } else {
        gpio_config_t cfg{};
        cfg.pin_bit_mask = 1ULL << CONFIG_LOCTEK_WAKE_GPIO;
        cfg.mode         = GPIO_MODE_OUTPUT;
        cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type    = GPIO_INTR_DISABLE;
#if SOC_GPIO_SUPPORT_PIN_HYS_FILTER
        cfg.hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE;
#endif
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "wake gpio");
        return gpio_set_level(static_cast<gpio_num_t>(CONFIG_LOCTEK_WAKE_GPIO), 0);
    }
}

esp_err_t init_uart()
{
    const uart_config_t cfg = {
        .baud_rate           = BAUD_RATE,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
        .flags               = {},
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(UART, RX_BUF_SIZE, TX_BUF_SIZE, 0, nullptr, 0), TAG,
                        "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(UART, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART, CONFIG_LOCTEK_TX_GPIO, CONFIG_LOCTEK_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pins");
    return uart_flush_input(UART);
}

esp_err_t create_queues()
{
    s_moves = xQueueCreateStatic(MAILBOX_DEPTH, sizeof(Move),
                                 reinterpret_cast<std::uint8_t *>(s_move_storage), &s_move_ctrl);
    s_gotos = xQueueCreateStatic(MAILBOX_DEPTH, sizeof(int),
                                 reinterpret_cast<std::uint8_t *>(s_goto_storage), &s_goto_ctrl);
    s_presses = xQueueCreateStatic(PRESS_QUEUE_LEN, sizeof(Press),
                                   reinterpret_cast<std::uint8_t *>(s_press_storage),
                                   &s_press_ctrl);
    ESP_RETURN_ON_FALSE(s_moves != nullptr && s_gotos != nullptr && s_presses != nullptr,
                        ESP_ERR_NO_MEM, TAG, "queues");
    return ESP_OK;
}

StackType_t *internal_stack(std::uint32_t words)
{
    return static_cast<StackType_t *>(
        heap_caps_malloc(words * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

esp_err_t start_tasks()
{
    s_rx_stack     = internal_stack(RX_STACK);
    s_driver_stack = internal_stack(DRIVER_STACK);
    ESP_RETURN_ON_FALSE(s_rx_stack != nullptr && s_driver_stack != nullptr, ESP_ERR_NO_MEM, TAG,
                        "task stacks");

    s_driver = xTaskCreateStaticPinnedToCore(driver_task, "loctek_drv", DRIVER_STACK, nullptr,
                                             TASK_PRIORITY, s_driver_stack, &s_driver_ctrl,
                                             TASK_CORE);
    ESP_RETURN_ON_FALSE(s_driver != nullptr, ESP_ERR_NO_MEM, TAG, "driver task");

    TaskHandle_t rx = xTaskCreateStaticPinnedToCore(rx_task, "loctek_rx", RX_STACK, nullptr,
                                                    TASK_PRIORITY, s_rx_stack, &s_rx_ctrl,
                                                    TASK_CORE);
    ESP_RETURN_ON_FALSE(rx != nullptr, ESP_ERR_NO_MEM, TAG, "rx task");
    return ESP_OK;
}

esp_err_t post_press(Press::Kind kind, Key key)
{
    ESP_RETURN_ON_FALSE(s_started.load(std::memory_order_acquire), ESP_ERR_INVALID_STATE, TAG,
                        "not started");
    const Press press{kind, key};
    if (xQueueSend(s_presses, &press, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    wake_driver();
    return ESP_OK;
}

}  // namespace

esp_err_t start(HeightHandler on_height)
{
    ESP_RETURN_ON_FALSE(!s_started.load(std::memory_order_acquire), ESP_ERR_INVALID_STATE, TAG,
                        "already started");

    s_on_height = on_height;
    load_run_on();
    ESP_LOGI(TAG, "run-on up %d mm, down %d mm", d_run_on[dir_index(Move::Up)],
             d_run_on[dir_index(Move::Down)]);

    ESP_RETURN_ON_ERROR(init_wake_gpio(), TAG, "wake");
    ESP_RETURN_ON_ERROR(init_uart(), TAG, "uart");
    ESP_RETURN_ON_ERROR(turn_on(), TAG, "wake line high");
    ESP_RETURN_ON_ERROR(create_queues(), TAG, "queues");
    ESP_RETURN_ON_ERROR(start_tasks(), TAG, "tasks");

    s_started.store(true, std::memory_order_release);

    ESP_LOGI(TAG, "uart%d tx=%d rx=%d wake=%d, desk %d-%d mm", CONFIG_LOCTEK_UART_NUM,
             CONFIG_LOCTEK_TX_GPIO, CONFIG_LOCTEK_RX_GPIO, CONFIG_LOCTEK_WAKE_GPIO,
             CONFIG_LOCTEK_MIN_HEIGHT_MM, CONFIG_LOCTEK_MAX_HEIGHT_MM);
    return ESP_OK;
}

esp_err_t request_move(Move direction)
{
    ESP_RETURN_ON_FALSE(s_started.load(std::memory_order_acquire), ESP_ERR_INVALID_STATE, TAG,
                        "not started");
    s_hand.store(direction != Move::Stop, std::memory_order_relaxed);
    xQueueOverwrite(s_moves, &direction);
    wake_driver();
    return ESP_OK;
}

bool in_range(int height_mm)
{
    return height_mm >= CONFIG_LOCTEK_MIN_HEIGHT_MM && height_mm <= CONFIG_LOCTEK_MAX_HEIGHT_MM;
}

esp_err_t goto_height(int height_mm)
{
    ESP_RETURN_ON_FALSE(s_started.load(std::memory_order_acquire), ESP_ERR_INVALID_STATE, TAG,
                        "not started");
    ESP_RETURN_ON_FALSE(in_range(height_mm), ESP_ERR_INVALID_ARG, TAG,
                        "%d mm is outside the desk's range", height_mm);
    ESP_RETURN_ON_FALSE(!s_hand.load(std::memory_order_relaxed), ESP_ERR_INVALID_STATE, TAG,
                        "a hand is on the keys");
    xQueueOverwrite(s_gotos, &height_mm);
    wake_driver();
    return ESP_OK;
}

int driving_to()
{
    return s_travelling_to.load(std::memory_order_relaxed);
}

Move motion()
{
    return static_cast<Move>(s_motion.load(std::memory_order_relaxed));
}

esp_err_t goto_preset(Preset preset)
{
    ESP_LOGI(TAG, "preset %d", static_cast<int>(preset) + 1);
    return post_press(Press::Kind::Key, key_for(preset));
}

esp_err_t store_preset(Preset preset)
{
    ESP_LOGI(TAG, "storing preset %d", static_cast<int>(preset) + 1);
    return post_press(Press::Kind::Store, key_for(preset));
}

esp_err_t wake()
{
    return post_press(Press::Kind::Wake, Key::None);
}

esp_err_t nudge()
{
    return post_press(Press::Kind::Nudge, Key::None);
}

int peek_raw(std::uint8_t *out, int capacity)
{
    const int count = s_raw_len < capacity ? s_raw_len : capacity;
    std::memcpy(out, s_raw, static_cast<std::size_t>(count));
    return count;
}

Stats stats()
{
    return {s_bytes_received.load(std::memory_order_relaxed),
            s_frames_decoded.load(std::memory_order_relaxed),
            s_height_frames.load(std::memory_order_relaxed),
            s_heights_decoded.load(std::memory_order_relaxed)};
}

}  // namespace loctek
