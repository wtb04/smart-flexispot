#include "media.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "jpeg.h"
#include "units.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace media {
namespace {
constexpr char TAG[] = "media";

constexpr int HTTP_TIMEOUT_MS  = 8 * units::kMsPerSecond;
constexpr int HTTP_BUFFER_SIZE = 2 * units::kBytesPerKiB;
constexpr int HTTP_OK          = 200;

constexpr std::size_t PATH_SIZE   = 320;
constexpr std::size_t ORIGIN_SIZE = 96;
constexpr std::size_t URL_SIZE    = 512;

// One shown while the next is drawn into.
constexpr int ART_BUFFERS = 2;

// A favourite's cover that failed waits this long, not the doubling of the
// playing one: they change rarely, and nobody is waiting on them.
constexpr std::int64_t PICK_RETRY_US = units::kUsPerMinute;

// Also how soon a failed cover is looked at again.
constexpr TickType_t WAKE_INTERVAL = pdMS_TO_TICKS(10 * units::kMsPerSecond);

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

ArtHandler     s_on_art      = nullptr;
PickArtHandler s_on_pick_art = nullptr;

std::uint8_t  *s_body = nullptr;  // the cover as downloaded
std::uint16_t *s_art[ART_BUFFERS] = {};
int            s_next             = 0;

char              s_wanted[PATH_SIZE] = {};
char              s_loaded[PATH_SIZE] = {};
SemaphoreHandle_t s_lock              = nullptr;
StaticSemaphore_t s_lock_ctrl;

TaskHandle_t s_task = nullptr;

char s_origin[ORIGIN_SIZE] = {};

struct PickCover {
    char           wanted[PATH_SIZE];
    char           loaded[PATH_SIZE];
    std::int64_t   retry_at;
    std::uint16_t *pixels;
};
PickCover *s_picks = nullptr;  // kPickCount of them, in PSRAM

void copy_path(char *dest, const char *path)
{
    std::strncpy(dest, path, PATH_SIZE);
    dest[PATH_SIZE - 1] = '\0';
}

std::size_t download(const char *url)
{
    esp_http_client_config_t cfg{};
    cfg.url                   = url;
    cfg.crt_bundle_attach     = esp_crt_bundle_attach;
    cfg.timeout_ms            = HTTP_TIMEOUT_MS;
    cfg.buffer_size           = HTTP_BUFFER_SIZE;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return 0;
    }

    std::size_t total = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t length = esp_http_client_fetch_headers(client);
        const int     status = esp_http_client_get_status_code(client);
        if (status == HTTP_OK && length <= static_cast<int64_t>(jpeg::kMaxInput)) {
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

/** Where a decoded cover goes, and how big. */
struct Target {
    std::uint16_t *pixels;
    int            side;
};

/** Nearest neighbour: a box filter would read every source pixel, not one in 16. */
void shrink(const std::uint16_t *src, Square from, int stride, Target to)
{
    for (int y = 0; y < to.side; ++y) {
        const std::uint16_t *row =
            src + static_cast<std::size_t>(from.top + y * from.side / to.side) * stride + from.left;
        std::uint16_t *out = to.pixels + static_cast<std::size_t>(y) * to.side;
        for (int x = 0; x < to.side; ++x) {
            out[x] = row[x * from.side / to.side];
        }
    }
}

void shrink_grey(const std::uint8_t *src, Square from, int stride, Target to)
{
    for (int y = 0; y < to.side; ++y) {
        const std::uint8_t *row =
            src + static_cast<std::size_t>(from.top + y * from.side / to.side) * stride + from.left;
        std::uint16_t *out = to.pixels + static_cast<std::size_t>(y) * to.side;
        for (int x = 0; x < to.side; ++x) {
            out[x] = jpeg::grey_to_rgb565(row[x * from.side / to.side]);
        }
    }
}

bool s_last_hardware = false;

void take_cover(const jpeg::Picture &picture, void *target)
{
    const Target to   = *static_cast<const Target *>(target);
    const Square from = middle(picture.width, picture.height);
    if (picture.grey) {
        shrink_grey(static_cast<const std::uint8_t *>(picture.pixels), from, picture.stride, to);
    } else {
        shrink(static_cast<const std::uint16_t *>(picture.pixels), from, picture.stride, to);
    }
    s_last_hardware = picture.hardware;
    ESP_LOGI(TAG, "cover %dx%d%s%s -> %d, stack left %u", picture.width, picture.height,
             picture.grey ? " grey" : "", picture.hardware ? "" : " in software", to.side,
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

bool decode(std::size_t bytes, Target to)
{
    const bool ok = jpeg::decode(s_body, bytes, jpeg::kMaxSide, jpeg::kMaxSide, take_cover, &to);
    if (!ok) {
        ESP_LOGW(TAG, "cover would not decode");
    }
    return ok;
}

bool fetch(const char *url, Target to)
{
    const std::size_t bytes = download(url);
    return bytes > 0 && decode(bytes, to);
}

bool fetch_playing(const char *path)
{
    // A path on Home Assistant, or a whole address, as a Jellyfin cover has.
    char       url[URL_SIZE];
    const bool whole = std::strncmp(path, "http", std::strlen("http")) == 0;
    std::snprintf(url, sizeof(url), "%s%s", whole ? "" : s_origin, path);
    std::uint16_t *art = s_art[s_next];
    if (!fetch(url, {art, kArtSize})) {
        return false;
    }
    s_next = (s_next + 1) % ART_BUFFERS;
    if (s_on_art != nullptr) {
        s_on_art(Art::Ready, art);
    }
    return true;
}

// A cover that failed is tried again, the wait doubling each time, rather than
// left blank until the track changes.
constexpr std::int64_t RETRY_FIRST_US = 20 * units::kUsPerSecond;
constexpr std::int64_t RETRY_MAX_US   = 5 * units::kUsPerMinute;

void clear_art()
{
    std::strcpy(s_loaded, "");
    record(false, false, false, true, -1);
    if (s_on_art != nullptr) {
        s_on_art(Art::None, nullptr);
    }
}

// The playing cover, when it changed and is not waiting out a failure.
struct Playing {
    char         failed[PATH_SIZE] = {};
    std::int64_t retry_at          = 0;
    std::int64_t retry_wait        = RETRY_FIRST_US;
};

void refresh_playing(Playing &playing)
{
    char wanted[PATH_SIZE];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_path(wanted, s_wanted);
    xSemaphoreGive(s_lock);

    if (std::strcmp(wanted, s_loaded) == 0) {
        return;
    }
    const bool again = std::strcmp(wanted, playing.failed) == 0;
    if (again && esp_timer_get_time() < playing.retry_at) {
        return;
    }
    if (wanted[0] == '\0') {
        clear_art();
        return;
    }

    const std::int64_t began = esp_timer_get_time();
    const bool         got   = fetch_playing(wanted);
    record(true, true, got, s_last_hardware,
           got ? static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs) : -1);

    if (got) {
        copy_path(s_loaded, wanted);
        playing.failed[0]  = '\0';
        playing.retry_wait = RETRY_FIRST_US;
        return;
    }
    if (!again) {
        copy_path(playing.failed, wanted);
        playing.retry_wait = RETRY_FIRST_US;
        if (s_on_art != nullptr) {
            s_on_art(Art::Failed, nullptr);
        }
    } else {
        playing.retry_wait = std::min(playing.retry_wait * 2, RETRY_MAX_US);
    }
    playing.retry_at = esp_timer_get_time() + playing.retry_wait;
    ESP_LOGW(TAG, "cover failed, trying again in %d s",
             static_cast<int>(playing.retry_wait / units::kUsPerSecond));
}

void refresh_picks()
{
    for (int i = 0; i < kPickCount; ++i) {
        PickCover &pick = s_picks[i];
        char       wanted[PATH_SIZE];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        copy_path(wanted, pick.wanted);
        xSemaphoreGive(s_lock);

        if (std::strcmp(wanted, pick.loaded) == 0 || esp_timer_get_time() < pick.retry_at) {
            continue;
        }
        const bool got = wanted[0] != '\0' && fetch(wanted, {pick.pixels, kPickArtSize});
        if (wanted[0] != '\0' && !got) {
            pick.retry_at = esp_timer_get_time() + PICK_RETRY_US;
            ESP_LOGW(TAG, "favourite %d's cover failed", i);
            continue;
        }
        copy_path(pick.loaded, wanted);
        if (s_on_pick_art != nullptr) {
            s_on_pick_art(i, got ? pick.pixels : nullptr);
        }
    }
}

[[noreturn]] void media_task(void *)
{
    Playing playing;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, WAKE_INTERVAL);
        refresh_playing(playing);
        refresh_picks();
    }
}

}  // namespace

esp_err_t start(const char *origin, ArtHandler on_art, PickArtHandler on_pick_art)
{
    std::snprintf(s_origin, sizeof(s_origin), "%s", origin != nullptr ? origin : "");
    s_on_art      = on_art;
    s_on_pick_art = on_pick_art;
    s_lock   = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, TAG, "lock");

    s_body = static_cast<std::uint8_t *>(heap_caps_malloc(jpeg::kMaxInput, MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(s_body != nullptr, ESP_ERR_NO_MEM, TAG, "cover buffer");

    for (auto &buffer : s_art) {
        buffer = static_cast<std::uint16_t *>(
            heap_caps_malloc(kArtSize * kArtSize * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM));
        ESP_RETURN_ON_FALSE(buffer != nullptr, ESP_ERR_NO_MEM, TAG, "art buffer");
    }

    s_picks = static_cast<PickCover *>(
        heap_caps_calloc(kPickCount, sizeof(PickCover), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_picks != nullptr, ESP_ERR_NO_MEM, TAG, "favourites");
    for (int i = 0; i < kPickCount; ++i) {
        s_picks[i].pixels = static_cast<std::uint16_t *>(heap_caps_malloc(
            kPickArtSize * kPickArtSize * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM));
        ESP_RETURN_ON_FALSE(s_picks[i].pixels != nullptr, ESP_ERR_NO_MEM, TAG, "favourite cover");
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
    const char *wanted  = path != nullptr ? path : "";
    const bool  changed = std::strncmp(s_wanted, wanted, sizeof(s_wanted)) != 0;
    if (changed) {
        copy_path(s_wanted, wanted);
    }
    xSemaphoreGive(s_lock);

    if (changed && s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void set_pick_art(int index, const char *url)
{
    if (s_lock == nullptr || s_picks == nullptr || index < 0 || index >= kPickCount) {
        return;
    }
    const char *wanted = url != nullptr ? url : "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = std::strncmp(s_picks[index].wanted, wanted, PATH_SIZE) != 0;
    if (changed) {
        copy_path(s_picks[index].wanted, wanted);
        s_picks[index].retry_at = 0;
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
