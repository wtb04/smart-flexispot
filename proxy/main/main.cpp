#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "link.h"
#include "loctek.h"

#include <cstdint>
#include <cstdio>
#include "nvs_flash.h"

namespace {
constexpr char TAG[] = "proxy";

int s_shown_mm = -1;

void on_height(int height_mm)
{
    desklink::note_height(height_mm);

    static std::int64_t last = 0;
    if (height_mm == s_shown_mm || esp_timer_get_time() - last < 500000) {
        return;
    }
    last       = esp_timer_get_time();
    s_shown_mm = height_mm;
    ESP_LOGI(TAG, "height %d.%d cm", height_mm / 10, height_mm % 10);
}
}  // namespace

extern "C" void app_main(void)
{
    if (esp_err_t err = nvs_flash_init();
        err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_init());
    }

    // Before the desk and the radio: if either fails to come up the LEDs are
    // how you find out, so they must already be running.
    ESP_ERROR_CHECK_WITHOUT_ABORT(deskled::start());

    ESP_ERROR_CHECK(loctek::start(on_height));
    ESP_LOGI(TAG, "desk link up, listening");
    ESP_ERROR_CHECK_WITHOUT_ABORT(desklink::start());

    std::uint32_t last_frames = 0;
    bool          complained  = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        const loctek::Stats stats = loctek::stats();

        if (stats.frames_decoded == 0) {
            std::uint8_t raw[48];
            const int got = loctek::peek_raw(raw, sizeof(raw));
            if (got > 0) {
                char hex[3 * sizeof(raw) + 1] = {};
                for (int i = 0; i < got; ++i) {
                    std::snprintf(hex + i * 3, 4, "%02x ", raw[i]);
                }
                ESP_LOGI(TAG, "raw: %s", hex);
            }
        }
        if (stats.frames_decoded == last_frames) {
            if (!complained) {
                complained = true;
                ESP_LOGW(TAG, "no frames from the control box (%u bytes seen)",
                         stats.bytes_received);
            }
        } else {
            complained  = false;
            last_frames = stats.frames_decoded;
        }
    }
}
