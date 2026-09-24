#include "media.h"

#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace media {
namespace {
constexpr char TAG[] = "media";

Status       s_status{};
portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

void record(bool playing, bool have_art, bool art_ok, bool hardware, int decode_ms)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.playing  = playing;
    s_status.have_art = have_art;
    s_status.art_ok   = art_ok;
    if (decode_ms >= 0) {
        s_status.hardware  = hardware;
        s_status.decode_ms = decode_ms;
        ++s_status.decodes;
    }
    portEXIT_CRITICAL(&s_status_lock);
}

// The software decoder runs on this stack when the engine cannot take a cover,
// as for a video's thumbnail, and needs far more than the fetch does. In PSRAM,
// since the task never touches flash, so it costs no internal RAM.
constexpr std::uint32_t TASK_STACK    = 12288;
constexpr UBaseType_t   TASK_PRIORITY = 2;
constexpr BaseType_t    TASK_CORE     = 0;

StaticTask_t s_task_ctrl;
StackType_t *s_task_stack = nullptr;

ArtHandler s_on_art = nullptr;

std::uint8_t *s_body = nullptr;  // the cover as downloaded
std::uint16_t *s_art[2] = {nullptr, nullptr};
int            s_next   = 0;

char              s_wanted[320] = {};
char              s_loaded[320] = {};
SemaphoreHandle_t s_lock        = nullptr;
StaticSemaphore_t s_lock_ctrl;

TaskHandle_t      s_task = nullptr;

char s_origin[96] = {};

std::size_t download(const char *path)
{
    char url[512];
    std::snprintf(url, sizeof(url), "%s%s", s_origin, path);

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
        if (status == 200 && length <= static_cast<int64_t>(jpeg::kMaxInput)) {
            while (total < jpeg::kMaxInput) {
                const int read = esp_http_client_read(client, reinterpret_cast<char *>(s_body + total),
                                                      static_cast<int>(jpeg::kMaxInput - total));
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

/** The card's frame is square; a wide episode still shows its middle. */
struct Square {
    int left;
    int top;
    int side;
};

Square middle(int width, int height)
{
    const int side = width < height ? width : height;
    return {(width - side) / 2, (height - side) / 2, side};
}

/** Nearest neighbour: a box filter would read every source pixel, not one in 16. */
void shrink(const std::uint16_t *src, Square from, int stride, std::uint16_t *dst)
{
    for (int y = 0; y < kArtSize; ++y) {
        const std::uint16_t *row =
            src + static_cast<std::size_t>(from.top + y * from.side / kArtSize) * stride + from.left;
        std::uint16_t *out = dst + static_cast<std::size_t>(y) * kArtSize;
        for (int x = 0; x < kArtSize; ++x) {
            out[x] = row[x * from.side / kArtSize];
        }
    }
}

void shrink_grey(const std::uint8_t *src, Square from, int stride, std::uint16_t *dst)
{
    for (int y = 0; y < kArtSize; ++y) {
        const std::uint8_t *row =
            src + static_cast<std::size_t>(from.top + y * from.side / kArtSize) * stride + from.left;
        std::uint16_t *out = dst + static_cast<std::size_t>(y) * kArtSize;
        for (int x = 0; x < kArtSize; ++x) {
            out[x] = jpeg::grey_to_rgb565(row[x * from.side / kArtSize]);
        }
    }
}

bool s_last_hardware = false;

void take_cover(const jpeg::Picture &picture, void *)
{
    const Square   from = middle(picture.width, picture.height);
    std::uint16_t *art  = s_art[s_next];
    if (picture.grey) {
        shrink_grey(static_cast<const std::uint8_t *>(picture.pixels), from, picture.stride, art);
    } else {
        shrink(static_cast<const std::uint16_t *>(picture.pixels), from, picture.stride, art);
    }
    s_next          = 1 - s_next;
    s_last_hardware = picture.hardware;
    ESP_LOGI(TAG, "cover %dx%d%s%s -> %d, stack left %u", picture.width, picture.height,
             picture.grey ? " grey" : "", picture.hardware ? "" : " in software", kArtSize,
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    if (s_on_art != nullptr) {
        s_on_art(Art::Ready, art);
    }
}

bool decode(std::size_t bytes)
{
    const bool ok = jpeg::decode(s_body, bytes, jpeg::kMaxSide, jpeg::kMaxSide, take_cover, nullptr);
    if (!ok) {
        ESP_LOGW(TAG, "cover would not decode");
    }
    return ok;
}

// A cover that failed is tried again, the wait doubling each time, rather than
// left blank until the track changes.
constexpr std::int64_t RETRY_FIRST_US = 20 * 1000000LL;
constexpr std::int64_t RETRY_MAX_US   = 5 * 60 * 1000000LL;

[[noreturn]] void media_task(void *)
{
    char         failed[sizeof(s_wanted)] = {};
    std::int64_t retry_at                 = 0;
    std::int64_t retry_wait               = RETRY_FIRST_US;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));

        char wanted[sizeof(s_wanted)];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        std::strncpy(wanted, s_wanted, sizeof(wanted));
        wanted[sizeof(wanted) - 1] = '\0';
        xSemaphoreGive(s_lock);

        if (std::strcmp(wanted, s_loaded) == 0) {
            continue;
        }
        const bool again = std::strcmp(wanted, failed) == 0;
        if (again && esp_timer_get_time() < retry_at) {
            continue;
        }
        ESP_LOGD(TAG, "cover path changed");
        if (wanted[0] == '\0') {
            std::strcpy(s_loaded, "");
            record(false, false, false, true, -1);
            if (s_on_art != nullptr) {
                s_on_art(Art::None, nullptr);
            }
            continue;
        }

        const std::int64_t began = esp_timer_get_time();
        const std::size_t  bytes = download(wanted);
        const bool         got   = bytes > 0 && decode(bytes);
        record(true, true, got, s_last_hardware,
               got ? static_cast<int>((esp_timer_get_time() - began) / 1000) : -1);

        if (got) {
            std::strncpy(s_loaded, wanted, sizeof(s_loaded));
            s_loaded[sizeof(s_loaded) - 1] = '\0';
            failed[0]  = '\0';
            retry_wait = RETRY_FIRST_US;
            continue;
        }
        if (!again) {
            std::strncpy(failed, wanted, sizeof(failed));
            failed[sizeof(failed) - 1] = '\0';
            retry_wait                 = RETRY_FIRST_US;
            if (s_on_art != nullptr) {
                s_on_art(Art::Failed, nullptr);
            }
        } else {
            retry_wait = std::min(retry_wait * 2, RETRY_MAX_US);
        }
        retry_at = esp_timer_get_time() + retry_wait;
        ESP_LOGW(TAG, "cover failed, trying again in %d s", static_cast<int>(retry_wait / 1000000));
    }
}

}  // namespace

esp_err_t start(const char *origin, ArtHandler on_art)
{
    std::snprintf(s_origin, sizeof(s_origin), "%s", origin != nullptr ? origin : "");
    s_on_art = on_art;
    s_lock   = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, TAG, "lock");

    s_body = static_cast<std::uint8_t *>(heap_caps_malloc(jpeg::kMaxInput, MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(s_body != nullptr, ESP_ERR_NO_MEM, TAG, "cover buffer");

    for (auto &buffer : s_art) {
        buffer = static_cast<std::uint16_t *>(
            heap_caps_malloc(kArtSize * kArtSize * 2, MALLOC_CAP_SPIRAM));
        ESP_RETURN_ON_FALSE(buffer != nullptr, ESP_ERR_NO_MEM, TAG, "art buffer");
    }

    s_task_stack = static_cast<StackType_t *>(
        heap_caps_malloc(TASK_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_task_stack != nullptr, ESP_ERR_NO_MEM, TAG, "task stack");
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


Status status()
{
    portENTER_CRITICAL(&s_status_lock);
    const Status copy = s_status;
    portEXIT_CRITICAL(&s_status_lock);
    return copy;
}

}  // namespace media
