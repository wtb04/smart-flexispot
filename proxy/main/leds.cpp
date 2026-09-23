#include "leds.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "loctek.h"

#include <cstdint>

namespace deskled {
namespace {
constexpr char TAG[] = "leds";

constexpr int BT_PIN   = CONFIG_PROXY_LED_BT_GPIO;
constexpr int DESK_PIN = CONFIG_PROXY_LED_DESK_GPIO;

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
    return (now_us / 1000) % period_ms < on_ms;
}

[[noreturn]] void led_task(void *)
{
    for (;;) {
        const std::int64_t now = esp_timer_get_time();

        // BT: solid once a panel is connected; a short heartbeat while
        // advertising, so "powered but nobody has connected" reads differently
        // from "not running at all".
        set(BT_PIN, desklink::panel_connected() || duty(now, 2000, 120));

        // LINK: solid while the box answers. Bytes arriving but nothing
        // decoding gets a fast blink -- that is a baud or wiring fault, and it
        // deserves to look different from silence.
        bool desk_on = false;
        if (desklink::box_up()) {
            desk_on = true;
        } else if (loctek::stats().bytes_received > 0) {
            desk_on = duty(now, 250, 125);
        }
        set(DESK_PIN, desk_on);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
}  // namespace

void selftest()
{
    for (int i = 0; i < 2; ++i) {
        set(BT_PIN, true);
        vTaskDelay(pdMS_TO_TICKS(220));
        set(BT_PIN, false);
        vTaskDelay(pdMS_TO_TICKS(140));
    }
    for (int i = 0; i < 2; ++i) {
        set(DESK_PIN, true);
        vTaskDelay(pdMS_TO_TICKS(220));
        set(DESK_PIN, false);
        vTaskDelay(pdMS_TO_TICKS(140));
    }
    set(BT_PIN, true);
    set(DESK_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(400));
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

    ESP_RETURN_ON_FALSE(xTaskCreate(led_task, "leds", 2560, nullptr, 2, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "led task");
    return ESP_OK;
}

}  // namespace deskled
