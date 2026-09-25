#include "leds.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "loctek.h"
#include "units.h"

#include <cstdint>

namespace deskled {
namespace {
constexpr char TAG[] = "leds";

constexpr int BT_PIN   = CONFIG_PROXY_LED_BT_GPIO;
constexpr int DESK_PIN = CONFIG_PROXY_LED_DESK_GPIO;

constexpr TickType_t REFRESH = pdMS_TO_TICKS(50);

constexpr int HEARTBEAT_PERIOD_MS = 2000;
constexpr int HEARTBEAT_ON_MS     = 120;
constexpr int GARBLED_PERIOD_MS   = 250;
constexpr int GARBLED_ON_MS       = GARBLED_PERIOD_MS / 2;

constexpr int        SELFTEST_BLINKS = 2;
constexpr TickType_t SELFTEST_ON     = pdMS_TO_TICKS(220);
constexpr TickType_t SELFTEST_OFF    = pdMS_TO_TICKS(140);
constexpr TickType_t SELFTEST_BOTH   = pdMS_TO_TICKS(400);

constexpr std::uint32_t TASK_STACK    = 2560;
constexpr UBaseType_t   TASK_PRIORITY = 2;

void set(int pin, bool on)
{
    if (pin < 0) {
        return;
    }
    gpio_set_level(static_cast<gpio_num_t>(pin), on ? 1 : 0);
}

esp_err_t configure(int pin)
{
    if (pin < 0) {
        return ESP_OK;
    }
    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << static_cast<unsigned>(pin);
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio %d", pin);
    return gpio_set_level(static_cast<gpio_num_t>(pin), 0);
}

/** On for `on_ms` of every `period_ms`, off a free-running clock rather than a
 *  counter, so the two LEDs keep their own phase and a missed tick does not
 *  accumulate. */
bool duty(std::int64_t now_us, int period_ms, int on_ms)
{
    return (now_us / units::kUsPerMs) % period_ms < on_ms;
}

[[noreturn]] void led_task(void *)
{
    for (;;) {
        const std::int64_t now = esp_timer_get_time();

        // BT: solid once a panel is connected; a short heartbeat while
        // advertising, so "powered but nobody has connected" reads differently
        // from "not running at all".
        set(BT_PIN, desklink::panel_connected() || duty(now, HEARTBEAT_PERIOD_MS, HEARTBEAT_ON_MS));

        // LINK: solid while the box answers. Bytes arriving but nothing
        // decoding gets a fast blink -- that is a baud or wiring fault, and it
        // deserves to look different from silence.
        bool desk_on = false;
        if (desklink::box_up()) {
            desk_on = true;
        } else if (loctek::stats().bytes_received > 0) {
            desk_on = duty(now, GARBLED_PERIOD_MS, GARBLED_ON_MS);
        }
        set(DESK_PIN, desk_on);

        vTaskDelay(REFRESH);
    }
}

void blink(int pin)
{
    for (int i = 0; i < SELFTEST_BLINKS; ++i) {
        set(pin, true);
        vTaskDelay(SELFTEST_ON);
        set(pin, false);
        vTaskDelay(SELFTEST_OFF);
    }
}
}  // namespace

void selftest()
{
    blink(BT_PIN);
    blink(DESK_PIN);
    set(BT_PIN, true);
    set(DESK_PIN, true);
    vTaskDelay(SELFTEST_BOTH);
    set(BT_PIN, false);
    set(DESK_PIN, false);
}

esp_err_t start()
{
    if (BT_PIN < 0 && DESK_PIN < 0) {
        ESP_LOGI(TAG, "no indicator LEDs configured");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(configure(BT_PIN), TAG, "bt led");
    ESP_RETURN_ON_ERROR(configure(DESK_PIN), TAG, "link led");
    ESP_LOGI(TAG, "bt=%d link=%d", BT_PIN, DESK_PIN);

#if CONFIG_PROXY_LED_SELFTEST
    ESP_LOGI(TAG, "self-test: BT twice, then LINK twice, then both");
    selftest();
#endif

    ESP_RETURN_ON_FALSE(
        xTaskCreate(led_task, "leds", TASK_STACK, nullptr, TASK_PRIORITY, nullptr) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "led task");
    return ESP_OK;
}

}  // namespace deskled
