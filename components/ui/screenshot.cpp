#include "screenshot.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <cstdint>
#include <cstdio>

namespace ui {
namespace {
constexpr char TAG[] = "shot";

// Quarter size. Enough to judge where things sit and how they line up, and a
// sixteenth of the bytes to push down a serial line.
constexpr int SHRINK = 3;

const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void emit(const std::uint8_t *bytes, std::size_t length)
{
    char line[121];
    int  at      = 0;
    int  written = 0;

    for (std::size_t i = 0; i < length; i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = i + 1 < length ? bytes[i + 1] : 0;
        const std::uint32_t c = i + 2 < length ? bytes[i + 2] : 0;
        const std::uint32_t triple = (a << 16) | (b << 8) | c;

        line[at++] = BASE64[(triple >> 18) & 0x3f];
        line[at++] = BASE64[(triple >> 12) & 0x3f];
        line[at++] = i + 1 < length ? BASE64[(triple >> 6) & 0x3f] : '=';
        line[at++] = i + 2 < length ? BASE64[triple & 0x3f] : '=';

        if (at >= 120) {
            line[at] = '\0';
            std::printf("SHOT %d %s\n", written, line);
            at = 0;

            // The console drops what it cannot take, and a picture missing a
            // line in the middle is not a picture.
            if (++written % 4 == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }
    if (at > 0) {
        line[at] = '\0';
        std::printf("SHOT %d %s\n", written, line);
    }
}

}  // namespace

void screenshot()
{
    if (!lvgl_port_lock(2000)) {
        ESP_LOGW(TAG, "lvgl lock");
        return;
    }
    lv_draw_buf_t *shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    lvgl_port_unlock();

    if (shot == nullptr) {
        ESP_LOGW(TAG, "no snapshot");
        return;
    }

    const std::int32_t out_w = shot->header.w / SHRINK;
    const std::int32_t out_h = shot->header.h / SHRINK;
    const std::size_t  bytes = static_cast<std::size_t>(out_w) * out_h * 2;

    // The whole picture is shrunk first and sent once. Encoding row by row pads
    // each row to a multiple of three, and a reader joining the rows back into
    // one stream gets every row after the first wrong.
    auto *small = static_cast<std::uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (small == nullptr) {
        lv_draw_buf_destroy(shot);
        return;
    }

    for (std::int32_t y = 0; y < out_h; ++y) {
        const auto *line = reinterpret_cast<const std::uint32_t *>(
            shot->data + static_cast<std::size_t>(y * SHRINK) * shot->header.stride);
        for (std::int32_t x = 0; x < out_w; ++x) {
            const std::uint32_t pixel = line[x * SHRINK];
            const std::uint32_t r     = (pixel >> 16) & 0xff;
            const std::uint32_t g     = (pixel >> 8) & 0xff;
            const std::uint32_t b     = pixel & 0xff;
            small[y * out_w + x] = static_cast<std::uint16_t>(((r & 0xf8) << 8) |
                                                              ((g & 0xfc) << 3) | (b >> 3));
        }
    }

    ESP_LOGI(TAG, "BEGIN %d %d", static_cast<int>(out_w), static_cast<int>(out_h));
    emit(reinterpret_cast<const std::uint8_t *>(small), bytes);
    ESP_LOGI(TAG, "END");

    heap_caps_free(small);
    lv_draw_buf_destroy(shot);
}

}  // namespace ui
