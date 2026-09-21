#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "loctek.h"

#include <cstdint>
#include <cstdio>
#include "nvs_flash.h"

namespace {
constexpr char TAG[] = "proxy";

// The box answers every frame, so this arrives about twenty times a second.
int s_shown_mm = -1;

void on_height(int height_mm)
{
    // Always passed on: the link wants every report, and the panel is where
    // this is meant to be read.
    desklink::note_height(height_mm);

    // The console only gets a couple a second. Eighteen a second of these is
    // most of what a 115200 line can carry, and it is the load the console
    // has misbehaved under.
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

    ESP_ERROR_CHECK(loctek::start(on_height));
    ESP_LOGI(TAG, "desk link up, listening");
    ESP_ERROR_CHECK_WITHOUT_ABORT(desklink::start());

    // Nothing drives the desk yet. This build only proves the wire: whether the
    // control box is answering at all, and whether what it says decodes.
    std::uint32_t last_frames = 0;
    bool          complained  = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        const loctek::Stats stats = loctek::stats();

        // Only when nothing is decoding, which is the only time it helps.
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
        // Nothing while it works. This line was printed once a minute and the
        // console then emitted that one call thousands of times over, the
        // same timestamp on every copy -- whatever is wrong is in the output
        // path rather than here, and the counters have done their job now
        // that the link is proven. What is worth saying is when the box goes
        // quiet.
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
