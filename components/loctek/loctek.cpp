#include "loctek.h"

#include "driver/gpio.h"
#include "nvs.h"
#include "soc/soc_caps.h"
#include "driver/uart.h"

#include <cstring>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace loctek {
namespace {
constexpr char TAG[] = "loctek";

constexpr auto UART        = static_cast<uart_port_t>(CONFIG_LOCTEK_UART_NUM);
constexpr int  BAUD_RATE   = 9600;
constexpr int  RX_BUF_SIZE = 1024;
constexpr int  TX_BUF_SIZE = 0;

constexpr TickType_t REPEAT_TICKS    = pdMS_TO_TICKS(CONFIG_LOCTEK_REPEAT_MS);
constexpr TickType_t IDLE_POLL_TICKS = pdMS_TO_TICKS(CONFIG_LOCTEK_IDLE_POLL_MS);
constexpr TickType_t MOVE_TIMEOUT    = pdMS_TO_TICKS(CONFIG_LOCTEK_MOVE_TIMEOUT_MS);
// A travel whose height stops changing has hit the end of the range, or lost
// the box, long before the travel timeout would notice.
constexpr TickType_t GOTO_STALL = pdMS_TO_TICKS(2500);

// The desk runs on after the last key frame. How far is learned from where each
// travel actually comes to rest, per direction, and kept across reboots.
constexpr int        RUN_ON_MAX_MM  = 40;
constexpr TickType_t SETTLED_GAP    = pdMS_TO_TICKS(400);
constexpr char       NVS_NAMESPACE[] = "loctek";
constexpr char       NVS_RUN_ON[]    = "runon";

constexpr std::uint32_t TX_TASK_STACK    = 3072;
constexpr std::uint32_t RX_TASK_STACK    = 3072;
constexpr UBaseType_t   TX_TASK_PRIORITY = 6;
constexpr UBaseType_t   RX_TASK_PRIORITY = 6;
constexpr BaseType_t    TASK_CORE        = 0;
constexpr UBaseType_t   MOVE_QUEUE_LEN   = 4;
constexpr int           kStopRepeats     = 3;

constexpr KeyFrame kFrameStop = build_key_frame(Key::None);
constexpr KeyFrame kFrameUp   = build_key_frame(Key::Up);
constexpr KeyFrame kFrameDown = build_key_frame(Key::Down);

struct Request {
    Move direction;
    bool steering;  // from a travel, so dropped if that travel has since been ended
};

StaticQueue_t s_move_queue_ctrl;
Request       s_move_queue_storage[MOVE_QUEUE_LEN];
QueueHandle_t s_move_queue = nullptr;

std::atomic<std::int8_t> s_motion{0};
std::atomic<int>         s_goto_target{-1};
std::atomic<int>         s_last_height{-1};
std::atomic<TickType_t>  s_progress_at{0};  // when the height last changed

int s_run_on_mm[2] = {0, 0};  // up, down

// After a travel releases, the landing is watched until the height settles.
std::atomic<int> s_landing_target{-1};
Move             s_landing_dir     = Move::Stop;
int              s_landing_last    = -1;
TickType_t       s_landing_last_at = 0;

int dir_index(Move direction)
{
    return direction == Move::Down ? 1 : 0;
}

const char *dir_name(Move direction)
{
    return direction == Move::Down ? "down" : "up";
}

void load_run_on()
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    int         stored[2];
    std::size_t size = sizeof(stored);
    if (nvs_get_blob(handle, NVS_RUN_ON, stored, &size) == ESP_OK && size == sizeof(stored)) {
        for (int i = 0; i < 2; ++i) {
            s_run_on_mm[i] = std::clamp(stored[i], 0, RUN_ON_MAX_MM);
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
    if (nvs_set_blob(handle, NVS_RUN_ON, s_run_on_mm, sizeof(s_run_on_mm)) == ESP_OK) {
        nvs_commit(handle);
    }
    nvs_close(handle);
}

void land(int height_mm)
{
    const int target = s_landing_target.load(std::memory_order_relaxed);
    if (target < 0) {
        return;
    }
    const TickType_t now = xTaskGetTickCount();
    if (height_mm != s_landing_last || now - s_landing_last_at < SETTLED_GAP) {
        if (height_mm != s_landing_last) {
            s_landing_last    = height_mm;
            s_landing_last_at = now;
        }
        return;
    }
    s_landing_target.store(-1, std::memory_order_relaxed);

    const int past = s_landing_dir == Move::Up ? height_mm - target : target - height_mm;
    if (past == 0) {
        ESP_LOGI(TAG, "landed on %d mm", target);
        return;
    }
    // Half the miss at a time: above a metre the box reports whole centimetres,
    // and taking all of one would overshoot the other way next time.
    int      &run_on = s_run_on_mm[dir_index(s_landing_dir)];
    const int was    = run_on;
    run_on = std::clamp(run_on + (past + (past > 0 ? 1 : -1)) / 2, 0, RUN_ON_MAX_MM);
    ESP_LOGI(TAG, "landed %d mm %s %d mm; run-on %s is now %d mm", std::abs(past),
             past > 0 ? "past" : "short of", target, dir_name(s_landing_dir), run_on);
    if (run_on != was) {
        save_run_on();
    }
}

StaticSemaphore_t s_tx_mutex_ctrl;
SemaphoreHandle_t s_tx_mutex = nullptr;

StaticTask_t s_tx_task_ctrl;
StaticTask_t s_rx_task_ctrl;
StackType_t *s_tx_task_stack = nullptr;
StackType_t *s_rx_task_stack = nullptr;

HeightHandler s_on_height = nullptr;

std::atomic<std::uint32_t> s_bytes_received{0};
std::atomic<std::uint32_t> s_frames_decoded{0};
std::atomic<std::uint32_t> s_height_frames{0};
std::atomic<std::uint32_t> s_heights_decoded{0};

esp_err_t write_frame_locked(const KeyFrame &frame)
{
    const int written = uart_write_bytes(UART, frame.data(), frame.size());
    if (written != static_cast<int>(frame.size())) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(UART, pdMS_TO_TICKS(100));
}

esp_err_t write_frame(const KeyFrame &frame)
{
    if (s_tx_mutex == nullptr) {
        return write_frame_locked(frame);
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    const esp_err_t err = write_frame_locked(frame);
    xSemaphoreGive(s_tx_mutex);
    return err;
}

esp_err_t press_key(Key key, int duration_ms)
{
    const KeyFrame frame = build_key_frame(key);
    if (s_tx_mutex != nullptr) {
        xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    }

    esp_err_t        err = ESP_OK;
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    do {
        err = write_frame_locked(frame);
        vTaskDelay(REPEAT_TICKS);
    } while (err == ESP_OK && static_cast<std::int32_t>(xTaskGetTickCount() - end) < 0);

    const esp_err_t release = write_frame_locked(kFrameStop);
    if (s_tx_mutex != nullptr) {
        xSemaphoreGive(s_tx_mutex);
    }
    return err != ESP_OK ? err : release;
}

esp_err_t queue_move(Move direction, bool steering)
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    const Request request{direction, steering};
    if (xQueueSend(s_move_queue, &request, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_motion.store(static_cast<std::int8_t>(direction), std::memory_order_relaxed);
    return ESP_OK;
}

void end_travel()
{
    s_landing_target.store(-1, std::memory_order_relaxed);
    if (s_goto_target.exchange(-1, std::memory_order_relaxed) >= 0) {
        ESP_LOGI(TAG, "travel ended");
    }
}

/** Ends a travel and releases the keys, before a key press of another kind. */
void settle()
{
    end_travel();
    if (s_motion.load(std::memory_order_relaxed) != 0) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(queue_move(Move::Stop, false));
    }
}

void steer_toward(int height_mm)
{
    const int target = s_goto_target.load(std::memory_order_relaxed);
    if (target < 0) {
        land(height_mm);
        return;
    }
    const auto current = static_cast<Move>(s_motion.load(std::memory_order_relaxed));
    const Move heading = current != Move::Stop ? current
                         : target > height_mm  ? Move::Up
                                               : Move::Down;
    const Move next = steer(target, height_mm, current, s_run_on_mm[dir_index(heading)]);
    if (next == Move::Stop) {
        ESP_LOGI(TAG, "releasing at %d mm for %d mm", height_mm, target);
        s_goto_target.store(-1, std::memory_order_relaxed);
        if (current != Move::Stop) {
            s_landing_dir     = current;
            s_landing_last    = -1;
            s_landing_last_at = 0;
            s_landing_target.store(target, std::memory_order_relaxed);
        }
    }
    if (next != current) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(queue_move(next, true));
    }
}

const KeyFrame &frame_for(Move direction)
{
    switch (direction) {
        case Move::Up:   return kFrameUp;
        case Move::Down: return kFrameDown;
        case Move::Stop: break;
    }
    return kFrameStop;
}

[[noreturn]] void tx_task(void *)
{
    Move       direction     = Move::Stop;
    TickType_t move_deadline = 0;

    for (;;) {
        if (direction != Move::Stop) {
            const TickType_t now = xTaskGetTickCount();
            if (static_cast<std::int32_t>(now - move_deadline) >= 0) {
                ESP_LOGW(TAG, "travel timeout, stopping");
                end_travel();
                direction = Move::Stop;
            } else if (s_goto_target.load(std::memory_order_relaxed) >= 0 &&
                       now - s_progress_at.load(std::memory_order_relaxed) > GOTO_STALL) {
                ESP_LOGW(TAG, "height stopped changing, stopping");
                end_travel();
                direction = Move::Stop;
            }
            if (direction == Move::Stop) {
                s_motion.store(0, std::memory_order_relaxed);
            }
        }

        if (direction != Move::Stop || IDLE_POLL_TICKS > 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(frame_for(direction)));
        }

        Request          request{};
        const TickType_t wait =
            direction != Move::Stop ? REPEAT_TICKS
                                    : (IDLE_POLL_TICKS > 0 ? IDLE_POLL_TICKS : portMAX_DELAY);
        if (xQueueReceive(s_move_queue, &request, wait) != pdTRUE) {
            continue;
        }
        // A hand may have ended the travel between the steer and now.
        const bool stale = request.steering && request.direction != Move::Stop &&
                           s_goto_target.load(std::memory_order_relaxed) < 0;
        if (!stale && request.direction != direction) {
            const bool stopping = request.direction == Move::Stop;
            direction           = request.direction;
            if (direction != Move::Stop) {
                move_deadline = xTaskGetTickCount() + MOVE_TIMEOUT;
            } else if (stopping) {
                for (int i = 0; i < kStopRepeats; ++i) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(kFrameStop));
                }
            }
        }
        s_motion.store(static_cast<std::int8_t>(direction), std::memory_order_relaxed);
    }
}

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

[[noreturn]] void rx_task(void *)
{
    Parser parser;
    std::array<std::uint8_t, 64> buf{};

    for (;;) {
        const int read = uart_read_bytes(UART, buf.data(), buf.size(), pdMS_TO_TICKS(10));
        if (read > 0) {
            keep_raw(buf.data(), read);
        }
        if (read > 0) {
            s_bytes_received.fetch_add(static_cast<std::uint32_t>(read), std::memory_order_relaxed);
        }
        for (int i = 0; i < read; ++i) {
            const std::optional<Frame> frame = parser.push(buf[i]);
            if (!frame) {
                continue;
            }
            s_frames_decoded.fetch_add(1, std::memory_order_relaxed);
            if (frame->type() == FrameType::Height) {
                s_height_frames.fetch_add(1, std::memory_order_relaxed);
            }

            const std::optional<int> height_mm = decode_height_mm(*frame);
            if (height_mm) {
                s_heights_decoded.fetch_add(1, std::memory_order_relaxed);
                if (s_last_height.exchange(*height_mm, std::memory_order_relaxed) != *height_mm) {
                    s_progress_at.store(xTaskGetTickCount(), std::memory_order_relaxed);
                }
                steer_toward(*height_mm);
                if (s_on_height != nullptr) {
                    s_on_height(*height_mm);
                }
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

constexpr TickType_t WAKE_LOW = pdMS_TO_TICKS(200);

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

}  // namespace

esp_err_t start(HeightHandler on_height)
{
    ESP_RETURN_ON_FALSE(s_move_queue == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_on_height = on_height;
    load_run_on();
    ESP_LOGI(TAG, "run-on up %d mm, down %d mm", s_run_on_mm[0], s_run_on_mm[1]);

    ESP_RETURN_ON_ERROR(init_wake_gpio(), TAG, "wake");
    ESP_RETURN_ON_ERROR(init_uart(), TAG, "uart");
    ESP_RETURN_ON_ERROR(turn_on(), TAG, "wake line high");

    QueueHandle_t queue = xQueueCreateStatic(MOVE_QUEUE_LEN, sizeof(Request),
                                             reinterpret_cast<std::uint8_t *>(s_move_queue_storage),
                                             &s_move_queue_ctrl);
    ESP_RETURN_ON_FALSE(queue != nullptr, ESP_ERR_NO_MEM, TAG, "queue");
    s_move_queue = queue;
    s_tx_mutex   = xSemaphoreCreateMutexStatic(&s_tx_mutex_ctrl);
    ESP_RETURN_ON_FALSE(s_tx_mutex != nullptr, ESP_ERR_NO_MEM, TAG, "tx mutex");

    s_rx_task_stack = static_cast<StackType_t *>(
        heap_caps_malloc(RX_TASK_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_tx_task_stack = static_cast<StackType_t *>(
        heap_caps_malloc(TX_TASK_STACK * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_rx_task_stack != nullptr && s_tx_task_stack != nullptr, ESP_ERR_NO_MEM,
                        TAG, "task stacks");

    TaskHandle_t rx = xTaskCreateStaticPinnedToCore(rx_task, "loctek_rx", RX_TASK_STACK, nullptr,
                                                    RX_TASK_PRIORITY, s_rx_task_stack,
                                                    &s_rx_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(rx != nullptr, ESP_ERR_NO_MEM, TAG, "rx task");

    TaskHandle_t tx = xTaskCreateStaticPinnedToCore(tx_task, "loctek_tx", TX_TASK_STACK, nullptr,
                                                    TX_TASK_PRIORITY, s_tx_task_stack,
                                                    &s_tx_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(tx != nullptr, ESP_ERR_NO_MEM, TAG, "tx task");

    ESP_LOGI(TAG, "uart%d tx=%d rx=%d wake=%d", CONFIG_LOCTEK_UART_NUM, CONFIG_LOCTEK_TX_GPIO,
             CONFIG_LOCTEK_RX_GPIO, CONFIG_LOCTEK_WAKE_GPIO);
    return ESP_OK;
}

esp_err_t request_move(Move direction)
{
    end_travel();
    return queue_move(direction, false);
}

esp_err_t goto_height(int height_mm)
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    const int here = s_last_height.load(std::memory_order_relaxed);
    ESP_RETURN_ON_FALSE(here >= 0, ESP_ERR_INVALID_STATE, TAG, "height unknown");
    ESP_LOGI(TAG, "travelling from %d mm to %d mm", here, height_mm);
    s_progress_at.store(xTaskGetTickCount(), std::memory_order_relaxed);
    s_goto_target.store(height_mm, std::memory_order_relaxed);
    steer_toward(here);
    return ESP_OK;
}

int driving_to()
{
    return s_goto_target.load(std::memory_order_relaxed);
}

Move motion()
{
    return static_cast<Move>(s_motion.load(std::memory_order_relaxed));
}

esp_err_t wake()
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_ERROR(turn_on(), TAG, "turn on");
    return write_frame(kFrameStop);
}

esp_err_t nudge()
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_ERROR(write_frame(kFrameUp), TAG, "nudge");
    return write_frame(kFrameStop);
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

esp_err_t goto_preset(Preset preset)
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    settle();
    ESP_LOGI(TAG, "preset %d", static_cast<int>(preset) + 1);
    return press_key(key_for(preset), CONFIG_LOCTEK_PRESS_MS);
}

esp_err_t store_preset(Preset preset)
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    settle();
    ESP_LOGI(TAG, "storing preset %d", static_cast<int>(preset) + 1);

    static_assert(CONFIG_LOCTEK_PRESS_MS < 2000, "M must not be held near the factory-reset time");
    ESP_RETURN_ON_ERROR(press_key(Key::Memory, CONFIG_LOCTEK_PRESS_MS), TAG, "memory key");
    vTaskDelay(pdMS_TO_TICKS(CONFIG_LOCTEK_STORE_GAP_MS));
    return press_key(key_for(preset), CONFIG_LOCTEK_PRESS_MS);
}

esp_err_t send_key(Key key)
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    return write_frame(build_key_frame(key));
}

}  // namespace loctek
