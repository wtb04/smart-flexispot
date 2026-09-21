#include "media.h"

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hass_secrets.h"

#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>

namespace media {
namespace {

constexpr char TAG[] = "media";

// Covers are a few hundred kilobytes at most; anything larger is refused.
constexpr std::size_t MAX_JPEG = 512 * 1024;

// The decoder writes full size, so this is sized for the largest cover worth
// decoding, not for the square that ends up on screen. Spotify serves 640.
constexpr int MAX_DECODE_SIDE = 800;

constexpr std::uint32_t TASK_STACK    = 5120;
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t  s_task_stack[TASK_STACK];

ArtHandler s_on_art = nullptr;

jpeg_decoder_handle_t s_decoder = nullptr;
std::uint8_t         *s_jpeg    = nullptr;
std::uint8_t         *s_full    = nullptr;
// Two, so the cover on screen is never the one being written over.
std::uint16_t *s_art[2] = {nullptr, nullptr};
int            s_next   = 0;

// Compared so an unchanged track does not re-download its cover every tick.
char              s_wanted[320] = {};
char              s_loaded[320] = {};
SemaphoreHandle_t s_lock        = nullptr;
StaticSemaphore_t s_lock_ctrl;

// There is one JPEG engine on the part and one set of buffers behind it, so
// anything else wanting a picture decoded queues here.
SemaphoreHandle_t s_decoder_lock = nullptr;
StaticSemaphore_t s_decoder_lock_ctrl;
TaskHandle_t      s_task = nullptr;

const char *http_origin()
{
    static char origin[96];
    if (origin[0] != '\0') {
        return origin;
    }
    const char *uri = HASS_WS_URI;
    // ws://host/api/websocket -> http://host
    const char *host = std::strstr(uri, "://");
    host             = host != nullptr ? host + 3 : uri;
    const char *end  = std::strchr(host, '/');
    const std::size_t len = end != nullptr ? static_cast<std::size_t>(end - host) : std::strlen(host);

    const bool secure = std::strncmp(uri, "wss", 3) == 0;
    std::snprintf(origin, sizeof(origin), "%s%.*s", secure ? "https://" : "http://",
                  static_cast<int>(len), host);
    return origin;
}

std::size_t download(const char *path)
{
    char url[512];
    std::snprintf(url, sizeof(url), "%s%s", http_origin(), path);

    esp_http_client_config_t cfg{};
    cfg.url             = url;
    cfg.timeout_ms      = 8000;
    cfg.buffer_size     = 2048;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return 0;
    }

    std::size_t total = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t length = esp_http_client_fetch_headers(client);
        const int     status = esp_http_client_get_status_code(client);
        if (status == 200 && length <= static_cast<int64_t>(MAX_JPEG)) {
            while (total < MAX_JPEG) {
                const int read = esp_http_client_read(client, reinterpret_cast<char *>(s_jpeg + total),
                                                      static_cast<int>(MAX_JPEG - total));
                if (read <= 0) {
                    break;
                }
                total += static_cast<std::size_t>(read);
            }
        } else {
            ESP_LOGW(TAG, "cover http %d, %lld bytes", status, static_cast<long long>(length));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total;
}

// The decoder writes whole MCUs, so a cover whose width is not a multiple of
// the MCU width comes out with padding on the end of every row. Reading it back
// at the picture width slid each row a little further left than the one above,
// which shears the image into diagonal streaks of colour.
int decoded_stride(const jpeg_decode_picture_info_t &info)
{
    const int mcu_w = info.sample_method == JPEG_DOWN_SAMPLING_YUV422 ||
                              info.sample_method == JPEG_DOWN_SAMPLING_YUV420
                          ? 16
                          : 8;
    return (static_cast<int>(info.width) + mcu_w - 1) / mcu_w * mcu_w;
}

/** Nearest neighbour: a box filter would read every source pixel, not one in 16. */
void shrink(const std::uint16_t *src, int side, int stride, std::uint16_t *dst)
{
    for (int y = 0; y < kArtSize; ++y) {
        const std::uint16_t *row = src + static_cast<std::size_t>(y * side / kArtSize) * stride;
        std::uint16_t       *out = dst + static_cast<std::size_t>(y) * kArtSize;
        for (int x = 0; x < kArtSize; ++x) {
            out[x] = row[x * side / kArtSize];
        }
    }
}

bool decode_locked(std::size_t bytes)
{
    jpeg_decode_picture_info_t info{};
    if (jpeg_decoder_get_info(s_jpeg, bytes, &info) != ESP_OK) {
        ESP_LOGW(TAG, "not a readable jpeg");
        return false;
    }
    if (info.width != info.height || static_cast<int>(info.width) > MAX_DECODE_SIDE) {
        ESP_LOGW(TAG, "cover is %ux%u, skipping", info.width, info.height);
        return false;
    }

    jpeg_decode_cfg_t cfg{};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // Despite the name this picks the byte order of the RGB565 word, not the
    // channel order. _RGB writes it big-endian; LVGL reads it as a native
    // little-endian uint16, which mangles red and blue into each other and
    // splits green. Greys survive that, which is why it looked plausible until
    // a colourful cover turned up.
    cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

    std::uint32_t out_size = 0;
    const esp_err_t err = jpeg_decoder_process(s_decoder, &cfg, s_jpeg, bytes, s_full,
                                               MAX_DECODE_SIDE * MAX_DECODE_SIDE * 2, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode failed: %s", esp_err_to_name(err));
        return false;
    }

    std::uint16_t *art = s_art[s_next];
    shrink(reinterpret_cast<const std::uint16_t *>(s_full), static_cast<int>(info.width),
           decoded_stride(info), art);
    s_next = 1 - s_next;

    if (s_on_art != nullptr) {
        s_on_art(Art::Ready, art);
    }
    ESP_LOGI(TAG, "cover %ux%u stride %d -> %d", info.width, info.height, decoded_stride(info),
             kArtSize);
    return true;
}

bool decode(std::size_t bytes)
{
    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    const bool ok = decode_locked(bytes);
    xSemaphoreGive(s_decoder_lock);
    return ok;
}

[[noreturn]] void media_task(void *)
{
    for (;;) {
        // Woken by set_art_path; the timeout is only a safety net.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));

        char wanted[sizeof(s_wanted)];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        std::strncpy(wanted, s_wanted, sizeof(wanted));
        wanted[sizeof(wanted) - 1] = '\0';
        xSemaphoreGive(s_lock);

        if (std::strcmp(wanted, s_loaded) == 0) {
            continue;
        }
        if (wanted[0] == '\0') {
            std::strcpy(s_loaded, "");
            if (s_on_art != nullptr) {
                s_on_art(Art::None, nullptr);
            }
            continue;
        }

        const std::size_t bytes = download(wanted);
        const bool        got   = bytes > 0 && decode(bytes);

        // Recorded either way. Leaving it unrecorded meant a cover that could
        // not be fetched was retried every ten seconds for the whole track,
        // and the previous track's cover stayed on screen the entire time --
        // which is worse than showing nothing, because it is wrong.
        std::strncpy(s_loaded, wanted, sizeof(s_loaded));
        s_loaded[sizeof(s_loaded) - 1] = '\0';
        if (!got && s_on_art != nullptr) {
            s_on_art(Art::Failed, nullptr);
        }
    }
}

}  // namespace

bool decode_image(const void *jpeg, std::size_t length, std::uint16_t *out, int max_w, int max_h,
                  int &out_w, int &out_h)
{
    if (jpeg == nullptr || out == nullptr || length == 0 || length > MAX_JPEG ||
        s_decoder == nullptr) {
        return false;
    }

    xSemaphoreTake(s_decoder_lock, portMAX_DELAY);
    bool ok = false;

    // The engine reads over DMA from its own allocation, so the bytes have to
    // be moved into it rather than decoded where they landed.
    std::memcpy(s_jpeg, jpeg, length);

    // The engine refuses a picture whose width times height is not a multiple
    // of eight, and refuses it loudly from inside the driver. Roughly one
    // aircraft thumbnail in five is such a picture, which is a normal thing to
    // come across rather than three lines of error.
    jpeg_decode_picture_info_t info{};
    if (jpeg_decoder_get_info(s_jpeg, length, &info) == ESP_OK &&
        (info.width * info.height) % 8 == 0 &&
        static_cast<int>(info.width) <= max_w && static_cast<int>(info.height) <= max_h &&
        static_cast<int>(info.width) <= MAX_DECODE_SIDE &&
        static_cast<int>(info.height) <= MAX_DECODE_SIDE) {
        jpeg_decode_cfg_t cfg{};
        cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
        cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;

        std::uint32_t produced = 0;
        if (jpeg_decoder_process(s_decoder, &cfg, s_jpeg, length, s_full,
                                 MAX_DECODE_SIDE * MAX_DECODE_SIDE * 2, &produced) == ESP_OK) {
            const auto *src    = reinterpret_cast<const std::uint16_t *>(s_full);
            const int   stride = decoded_stride(info);
            out_w              = static_cast<int>(info.width);
            out_h              = static_cast<int>(info.height);
            for (int y = 0; y < out_h; ++y) {
                std::memcpy(out + static_cast<std::size_t>(y) * out_w,
                            src + static_cast<std::size_t>(y) * stride,
                            static_cast<std::size_t>(out_w) * 2);
            }
            ok = true;
        }
    }

    xSemaphoreGive(s_decoder_lock);
    return ok;
}

esp_err_t start(ArtHandler on_art)
{
    s_on_art = on_art;
    s_lock   = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, TAG, "lock");

    s_decoder_lock = xSemaphoreCreateMutexStatic(&s_decoder_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_decoder_lock != nullptr, ESP_ERR_NO_MEM, TAG, "decoder lock");

    const jpeg_decode_engine_cfg_t engine{.intr_priority = 0, .timeout_ms = 2000};
    ESP_RETURN_ON_ERROR(jpeg_new_decoder_engine(&engine, &s_decoder), TAG, "decoder");

    // Read and written over DMA, so they come from the decoder's own allocator.
    jpeg_decode_memory_alloc_cfg_t in{.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    jpeg_decode_memory_alloc_cfg_t out{.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    std::size_t got = 0;
    s_jpeg = static_cast<std::uint8_t *>(jpeg_alloc_decoder_mem(MAX_JPEG, &in, &got));
    s_full = static_cast<std::uint8_t *>(
        jpeg_alloc_decoder_mem(MAX_DECODE_SIDE * MAX_DECODE_SIDE * 2, &out, &got));
    ESP_RETURN_ON_FALSE(s_jpeg != nullptr && s_full != nullptr, ESP_ERR_NO_MEM, TAG, "buffers");

    for (auto &buffer : s_art) {
        buffer = static_cast<std::uint16_t *>(
            heap_caps_malloc(kArtSize * kArtSize * 2, MALLOC_CAP_SPIRAM));
        ESP_RETURN_ON_FALSE(buffer != nullptr, ESP_ERR_NO_MEM, TAG, "art buffer");
    }

    s_task = xTaskCreateStaticPinnedToCore(media_task, "media", TASK_STACK, nullptr, TASK_PRIORITY,
                                           s_task_stack, &s_task_ctrl, TASK_CORE);
    ESP_RETURN_ON_FALSE(s_task != nullptr, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

void set_art_path(const char *path)
{
    if (s_lock == nullptr) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = std::strncmp(s_wanted, path != nullptr ? path : "", sizeof(s_wanted)) != 0;
    if (changed) {
        std::strncpy(s_wanted, path != nullptr ? path : "", sizeof(s_wanted));
        s_wanted[sizeof(s_wanted) - 1] = '\0';
    }
    xSemaphoreGive(s_lock);

    if (changed && s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

}  // namespace media
