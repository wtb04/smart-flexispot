#include "loctek.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"


#include <atomic>

namespace loctek {
namespace {

constexpr char TAG[] = "loctek";

constexpr auto UART        = static_cast<uart_port_t>(CONFIG_LOCTEK_UART_NUM);
constexpr int  BAUD_RATE   = 9600;
constexpr int  RX_BUF_SIZE = 1024;
// No TX ring buffer: a backlog of movement frames cannot keep draining after
// the release frame is written.
constexpr int  TX_BUF_SIZE = 0;

constexpr TickType_t REPEAT_TICKS    = pdMS_TO_TICKS(CONFIG_LOCTEK_REPEAT_MS);
constexpr TickType_t IDLE_POLL_TICKS = pdMS_TO_TICKS(CONFIG_LOCTEK_IDLE_POLL_MS);
constexpr TickType_t MOVE_TIMEOUT    = pdMS_TO_TICKS(CONFIG_LOCTEK_MOVE_TIMEOUT_MS);

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

StaticQueue_t s_move_queue_ctrl;
Move          s_move_queue_storage[MOVE_QUEUE_LEN];
QueueHandle_t s_move_queue = nullptr;

StaticSemaphore_t s_tx_mutex_ctrl;
SemaphoreHandle_t s_tx_mutex = nullptr;

StaticTask_t s_tx_task_ctrl;
StackType_t  s_tx_task_stack[TX_TASK_STACK];
StaticTask_t s_rx_task_ctrl;
StackType_t  s_rx_task_stack[RX_TASK_STACK];

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

// Preset commands come from another task than the transmit loop, and two
// writers on one UART can interleave bytes mid-frame.
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

// A key press is a stream of frames, not one: a preset sent once registered
// only sometimes. The lock is held for the whole press so the idle poll cannot
// land in the middle of it and read as an instant release.
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


const KeyFrame &frame_for(Move direction)
{
    switch (direction) {
        case Move::Up:   return kFrameUp;
        case Move::Down: return kFrameDown;
        case Move::Stop: break;
    }
    return kFrameStop;
}

// The desk travels one short step per frame, so a held button means
// retransmitting. Releasing sends the "no keys pressed" frame rather than going
// quiet, or the desk coasts on.
[[noreturn]] void tx_task(void *)
{
    Move       direction     = Move::Stop;
    TickType_t move_deadline = 0;

    for (;;) {
        if (direction != Move::Stop &&
            static_cast<std::int32_t>(xTaskGetTickCount() - move_deadline) >= 0) {
            ESP_LOGW(TAG, "travel timeout, stopping");
            direction = Move::Stop;
        }

        if (direction != Move::Stop || IDLE_POLL_TICKS > 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(frame_for(direction)));
        }

        Move requested = Move::Stop;
        const TickType_t wait =
            direction != Move::Stop ? REPEAT_TICKS
                                    : (IDLE_POLL_TICKS > 0 ? IDLE_POLL_TICKS : portMAX_DELAY);
        if (xQueueReceive(s_move_queue, &requested, wait) == pdTRUE && requested != direction) {
            const bool stopping = requested == Move::Stop && direction != Move::Stop;
            direction = requested;
            if (direction != Move::Stop) {
                // No wake pulse: a movement frame is a real key press and the
                // box acts on it whether or not its panel is lit.
                move_deadline = xTaskGetTickCount() + MOVE_TIMEOUT;
            } else if (stopping) {
                // Repeated so a single dropped frame cannot leave the desk
                // travelling on past the button release.
                for (int i = 0; i < kStopRepeats; ++i) {
                    ESP_ERROR_CHECK_WITHOUT_ABORT(write_frame(kFrameStop));
                }
            }
        }
    }
}

[[noreturn]] void rx_task(void *)
{
    Parser parser;
    // Small buffer, short timeout: the box sends ~525 B/s, and a read that
    // waits to fill batches two readings into one visible update.
    std::array<std::uint8_t, 64> buf{};

    for (;;) {
        const int read = uart_read_bytes(UART, buf.data(), buf.size(), pdMS_TO_TICKS(10));
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
                if (s_on_height != nullptr) {
                    s_on_height(*height_mm);
                }
            }
        }
    }
}

// The wake line rests low; driving it high puts the control box into operating
// mode. turnon() in https://github.com/iMicknl/LoctekMotion_IoT does the same.
esp_err_t init_wake_gpio()
{
    if constexpr (CONFIG_LOCTEK_WAKE_GPIO < 0) {
        return ESP_OK;
    } else {
        const gpio_config_t cfg = {
            .pin_bit_mask  = 1ULL << CONFIG_LOCTEK_WAKE_GPIO,
            .mode          = GPIO_MODE_OUTPUT,
            .pull_up_en    = GPIO_PULLUP_DISABLE,
            .pull_down_en  = GPIO_PULLDOWN_DISABLE,
            .intr_type     = GPIO_INTR_DISABLE,
            .hys_ctrl_mode = GPIO_HYS_SOFT_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "wake gpio");
        return gpio_set_level(static_cast<gpio_num_t>(CONFIG_LOCTEK_WAKE_GPIO), 0);
    }
}

// The line is left high, not returned low: a box held low goes fully silent and
// ignores movement frames. The low period must be long enough for the box to see
// the edge -- across a warm reset the line was already high and only floats
// briefly, so a couple of milliseconds low passes unnoticed and the panel stays
// dark.
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
    // The line floats until the pins are configured, so the FIFO can already
    // hold noise. Drop it rather than reporting a garbage link.
    return uart_flush_input(UART);
}

}  // namespace

esp_err_t start(HeightHandler on_height)
{
    ESP_RETURN_ON_FALSE(s_move_queue == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_on_height = on_height;

    ESP_RETURN_ON_ERROR(init_wake_gpio(), TAG, "wake");
    ESP_RETURN_ON_ERROR(init_uart(), TAG, "uart");
    // Unconditionally: the line has to end up high and stay there, or the box
    // goes quiet ten seconds later and stops acting on movement frames.
    ESP_RETURN_ON_ERROR(turn_on(), TAG, "wake line high");

    QueueHandle_t queue = xQueueCreateStatic(MOVE_QUEUE_LEN, sizeof(Move),
                                             reinterpret_cast<std::uint8_t *>(s_move_queue_storage),
                                             &s_move_queue_ctrl);
    ESP_RETURN_ON_FALSE(queue != nullptr, ESP_ERR_NO_MEM, TAG, "queue");
    s_move_queue = queue;
    s_tx_mutex   = xSemaphoreCreateMutexStatic(&s_tx_mutex_ctrl);
    ESP_RETURN_ON_FALSE(s_tx_mutex != nullptr, ESP_ERR_NO_MEM, TAG, "tx mutex");

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
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    return xQueueSend(s_move_queue, &direction, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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
    ESP_LOGI(TAG, "preset %d", static_cast<int>(preset) + 1);
    return press_key(key_for(preset), CONFIG_LOCTEK_PRESS_MS);
}

esp_err_t store_preset(Preset preset)
{
    ESP_RETURN_ON_FALSE(s_move_queue != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_LOGI(TAG, "storing preset %d", static_cast<int>(preset) + 1);

    // Five seconds of M puts the control box into factory reset, so this
    // duration must stay well clear of that.
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
