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

constexpr std::uint32_t LOCK_TIMEOUT_MS = 2000;

// Quarter size. Enough to judge where things sit and how they line up, and a
// sixteenth of the bytes to push down a serial line.
constexpr int SHRINK            = 2;
constexpr int PIXELS_PER_SAMPLE = SHRINK * SHRINK;

constexpr std::uint32_t CHANNEL_MASK = 0xff;
constexpr int           RED_SHIFT    = 16;
constexpr int           GREEN_SHIFT  = 8;

const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::size_t   BYTES_PER_GROUP = 3;
constexpr int           BITS_PER_CHAR   = 6;
constexpr std::uint32_t CHAR_MASK       = 0x3f;
constexpr char          PAD_CHAR        = '=';

constexpr std::size_t LINE_CAPACITY = 160;
constexpr int         LINE_CHARS    = 120;

// The console writes without blocking and throws away what will not fit.
// Installing the driver to make it block takes the peripheral off the console
// altogether and nothing comes out at all, so it is paced instead.
constexpr int           LINES_PER_PAUSE = 8;
constexpr std::uint32_t PAUSE_MS        = 10;
constexpr std::uint32_t BEGIN_PAUSE_MS  = 50;

char base64_char(std::uint32_t group, int position)
{
    const int shift = BITS_PER_CHAR * (static_cast<int>(BYTES_PER_GROUP) - position);
    return BASE64[(group >> shift) & CHAR_MASK];
}

int encode_group(const std::uint8_t *bytes, std::size_t i, std::size_t length, char *out)
{
    const std::uint32_t a     = bytes[i];
    const std::uint32_t b     = i + 1 < length ? bytes[i + 1] : 0;
    const std::uint32_t c     = i + 2 < length ? bytes[i + 2] : 0;
    const std::uint32_t group = (a << 16) | (b << 8) | c;

    int at    = 0;
    out[at++] = base64_char(group, 0);
    out[at++] = base64_char(group, 1);
    out[at++] = i + 1 < length ? base64_char(group, 2) : PAD_CHAR;
    out[at++] = i + 2 < length ? base64_char(group, 3) : PAD_CHAR;
    return at;
}

void print_line(char *line, int length, int number)
{
    line[length] = '\0';
    std::printf("SHOT %d %s\n", number, line);
}

void emit(const std::uint8_t *bytes, std::size_t length)
{
    char line[LINE_CAPACITY];
    int  at      = 0;
    int  written = 0;

    for (std::size_t i = 0; i < length; i += BYTES_PER_GROUP) {
        at += encode_group(bytes, i, length, line + at);
        if (at >= LINE_CHARS) {
            print_line(line, at, written);
            at = 0;
            if (++written % LINES_PER_PAUSE == 0) {
                vTaskDelay(pdMS_TO_TICKS(PAUSE_MS));
            }
        }
    }
    if (at > 0) {
        print_line(line, at, written);
    }
}

std::uint16_t to_rgb565(std::uint32_t r, std::uint32_t g, std::uint32_t b)
{
    return static_cast<std::uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

std::uint16_t average_block(const lv_draw_buf_t *shot, std::int32_t x, std::int32_t y)
{
    std::uint32_t r = 0;
    std::uint32_t g = 0;
    std::uint32_t b = 0;
    for (int dy = 0; dy < SHRINK; ++dy) {
        const auto *from = reinterpret_cast<const std::uint32_t *>(
            shot->data + static_cast<std::size_t>(y * SHRINK + dy) * shot->header.stride);
        for (int dx = 0; dx < SHRINK; ++dx) {
            const std::uint32_t pixel = from[x * SHRINK + dx];
            r += (pixel >> RED_SHIFT) & CHANNEL_MASK;
            g += (pixel >> GREEN_SHIFT) & CHANNEL_MASK;
            b += pixel & CHANNEL_MASK;
        }
    }
    return to_rgb565(r / PIXELS_PER_SAMPLE, g / PIXELS_PER_SAMPLE, b / PIXELS_PER_SAMPLE);
}

void shrink_into(const lv_draw_buf_t *shot, std::uint16_t *small, std::int32_t out_w,
                 std::int32_t out_h)
{
    for (std::int32_t y = 0; y < out_h; ++y) {
        for (std::int32_t x = 0; x < out_w; ++x) {
            small[y * out_w + x] = average_block(shot, x, y);
        }
    }
}

lv_draw_buf_t *take_snapshot()
{
    if (!lvgl_port_lock(LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "lvgl lock");
        return nullptr;
    }
    lv_draw_buf_t *shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    lvgl_port_unlock();
    if (shot == nullptr) {
        ESP_LOGW(TAG, "no snapshot");
    }
    return shot;
}

}  // namespace

void screenshot()
{
    lv_draw_buf_t *shot = take_snapshot();
    if (shot == nullptr) {
        return;
    }

    const std::int32_t out_w = shot->header.w / SHRINK;
    const std::int32_t out_h = shot->header.h / SHRINK;
    const std::size_t  bytes = static_cast<std::size_t>(out_w) * out_h * sizeof(std::uint16_t);

    // The whole picture is shrunk first and sent once. Encoding row by row pads
    // each row to a multiple of three, and a reader joining the rows back into
    // one stream gets every row after the first wrong.
    auto *small = static_cast<std::uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (small == nullptr) {
        lv_draw_buf_destroy(shot);
        return;
    }
    shrink_into(shot, small, out_w, out_h);

    ESP_LOGI(TAG, "BEGIN %d %d", static_cast<int>(out_w), static_cast<int>(out_h));
    vTaskDelay(pdMS_TO_TICKS(BEGIN_PAUSE_MS));
    emit(reinterpret_cast<const std::uint8_t *>(small), bytes);
    ESP_LOGI(TAG, "END");

    heap_caps_free(small);
    lv_draw_buf_destroy(shot);
}

}  // namespace ui
