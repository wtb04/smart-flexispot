#include "media.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "jpeg.h"
#include "net.h"
#include "units.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace media {
namespace {
constexpr char TAG[] = "media";

constexpr int HTTP_TIMEOUT_MS = 8 * units::kMsPerSecond;

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
StillHandler   s_on_still    = nullptr;

std::uint8_t  *s_body = nullptr;  // the cover as downloaded
std::uint16_t *s_art[ART_BUFFERS]   = {};
std::uint16_t *s_large[ART_BUFFERS] = {};  // the same covers, at kLargeArtSize
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
PickCover *s_still = nullptr;  // the same, for the wide still, of kStillW by kStillH

void copy_path(char *dest, const char *path)
{
    std::strncpy(dest, path, PATH_SIZE);
    dest[PATH_SIZE - 1] = '\0';
}

// Covers come from Home Assistant, from Jellyfin and from wherever a
// favourite's is kept: each origin a host of its own in net, found the first
// time it is asked for.
net::HostConfig cover_host()
{
    net::HostConfig config;
    config.timeout_ms  = HTTP_TIMEOUT_MS;
    config.connections = 1;
    config.idle_ms     = 15 * units::kMsPerSecond;  // asked in bursts, a track at a time
    config.retry       = net::Retry{1, 500, 200, false};
    config.rest        = net::Rest{3, 30 * units::kMsPerSecond, units::kMsPerMinute};
    return config;
}

std::size_t download(const char *url)
{
    net::Request request;
    request.host     = net::host_for(url, cover_host());
    request.path     = url;
    request.priority = net::Priority::Now;
    request.key      = url;
    request.dedupe   = net::Dedupe::Join;
    request.max_body = jpeg::kMaxInput - 1;  // and the end of text fetch() puts after it
    request.what     = "cover";
    const net::Fetched got = net::fetch(std::move(request), reinterpret_cast<char *>(s_body), jpeg::kMaxInput);
    if (!got.ok() || got.truncated || got.length >= jpeg::kMaxInput - 1) {
        ESP_LOGW(TAG, "cover %s, http %d%s", got.ok() ? "answered" : "not had", got.status,
                 got.truncated ? ", too large" : "");
        return 0;
    }
    return got.length;
}

/** Where a decoded cover goes, and how big; and a second size of it, if any. */
struct Target {
    std::uint16_t *pixels;
    int            w;
    int            h;
    const Target  *also     = nullptr;
    int           *tall_w   = nullptr;  // given, a tall picture is kept whole, as narrow as this says
};

// Taller than this share of its width, a picture is a poster, not a cover.
constexpr int TALL_PERCENT = 115;

/** The part of the picture kept: as much of its middle as has the target's
 *  shape, so a wide still shows its middle in a square frame and a square
 *  cover its middle band in a wide one. */
struct Crop {
    int left;
    int top;
    int w;
    int h;
};

Crop middle(int width, int height, Target to)
{
    int w = width;
    int h = width * to.h / to.w;
    if (h > height) {
        h = height;
        w = height * to.w / to.h;
    }
    return {(width - w) / 2, (height - h) / 2, w, h};
}

/** Nearest neighbour: a box filter would read every source pixel, not one in 16. */
void shrink(const std::uint16_t *src, Crop from, int stride, Target to)
{
    for (int y = 0; y < to.h; ++y) {
        const std::uint16_t *row =
            src + static_cast<std::size_t>(from.top + y * from.h / to.h) * stride + from.left;
        std::uint16_t *out = to.pixels + static_cast<std::size_t>(y) * to.w;
        for (int x = 0; x < to.w; ++x) {
            out[x] = row[x * from.w / to.w];
        }
    }
}

void shrink_grey(const std::uint8_t *src, Crop from, int stride, Target to)
{
    for (int y = 0; y < to.h; ++y) {
        const std::uint8_t *row =
            src + static_cast<std::size_t>(from.top + y * from.h / to.h) * stride + from.left;
        std::uint16_t *out = to.pixels + static_cast<std::size_t>(y) * to.w;
        for (int x = 0; x < to.w; ++x) {
            out[x] = jpeg::grey_to_rgb565(row[x * from.w / to.w]);
        }
    }
}

bool s_last_hardware = false;

void take_cover(const jpeg::Picture &picture, void *target)
{
    const Target to = *static_cast<const Target *>(target);
    for (const Target *size = &to; size != nullptr; size = size->also) {
        Target into = *size;
        if (into.tall_w != nullptr) {
            const bool tall = picture.height * 100 > picture.width * TALL_PERCENT;
            into.w          = tall ? std::max(1, into.h * picture.width / picture.height) : size->w;
            *into.tall_w    = into.w;
        }
        const Crop from = middle(picture.width, picture.height, into);
        if (picture.grey) {
            shrink_grey(static_cast<const std::uint8_t *>(picture.pixels), from, picture.stride, into);
        } else {
            shrink(static_cast<const std::uint16_t *>(picture.pixels), from, picture.stride, into);
        }
    }
    s_last_hardware = picture.hardware;
    ESP_LOGI(TAG, "cover %dx%d%s%s -> %dx%d, stack left %u", picture.width, picture.height,
             picture.grey ? " grey" : "", picture.hardware ? "" : " in software",
             to.tall_w != nullptr ? *to.tall_w : to.w, to.h,
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
    std::uint16_t *art   = s_art[s_next];
    std::uint16_t *large = s_large[s_next];
    const Target   big{large, kLargeArtSize, kLargeArtSize};
    int            width = kArtSize;
    if (!fetch(url, {art, kArtSize, kArtSize, large != nullptr ? &big : nullptr, &width})) {
        return false;
    }
    s_next = (s_next + 1) % ART_BUFFERS;
    if (s_on_art != nullptr) {
        s_on_art(Art::Ready, art, width, large);
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
        s_on_art(Art::None, nullptr, kArtSize, nullptr);
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
            s_on_art(Art::Failed, nullptr, kArtSize, nullptr);
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
        const bool got = wanted[0] != '\0' && fetch(wanted, {pick.pixels, kPickArtSize, kPickArtSize});
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

void refresh_still()
{
    char wanted[PATH_SIZE];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_path(wanted, s_still->wanted);
    xSemaphoreGive(s_lock);
    if (std::strcmp(wanted, s_still->loaded) == 0 || esp_timer_get_time() < s_still->retry_at) {
        return;
    }
    const bool got = wanted[0] != '\0' && fetch(wanted, {s_still->pixels, kStillW, kStillH});
    if (wanted[0] != '\0' && !got) {
        s_still->retry_at = esp_timer_get_time() + PICK_RETRY_US;
        return;
    }
    copy_path(s_still->loaded, wanted);
    if (s_on_still != nullptr) {
        s_on_still(got ? s_still->pixels : nullptr);
    }
}

[[noreturn]] void media_task(void *)
{
    Playing playing;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, WAKE_INTERVAL);
        refresh_playing(playing);
        refresh_still();
        refresh_picks();
    }
}

}  // namespace

esp_err_t start(const char *origin, ArtHandler on_art, PickArtHandler on_pick_art,
                StillHandler on_still)
{
    std::snprintf(s_origin, sizeof(s_origin), "%s", origin != nullptr ? origin : "");
    s_on_art      = on_art;
    s_on_pick_art = on_pick_art;
    s_on_still    = on_still;
    s_lock   = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_lock != nullptr, ESP_ERR_NO_MEM, TAG, "lock");

    s_body = static_cast<std::uint8_t *>(heap_caps_malloc(jpeg::kMaxInput, MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(s_body != nullptr, ESP_ERR_NO_MEM, TAG, "cover buffer");

    for (auto &buffer : s_art) {
        buffer = static_cast<std::uint16_t *>(
            heap_caps_malloc(kArtSize * kArtSize * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM));
        ESP_RETURN_ON_FALSE(buffer != nullptr, ESP_ERR_NO_MEM, TAG, "art buffer");
    }
    // Without room for them, the music view shows the card's cover instead.
    for (auto &buffer : s_large) {
        buffer = static_cast<std::uint16_t *>(
            heap_caps_malloc(kLargeArtSize * kLargeArtSize * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM));
    }

    s_picks = static_cast<PickCover *>(
        heap_caps_calloc(kPickCount, sizeof(PickCover), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_picks != nullptr, ESP_ERR_NO_MEM, TAG, "favourites");
    for (int i = 0; i < kPickCount; ++i) {
        s_picks[i].pixels = static_cast<std::uint16_t *>(heap_caps_malloc(
            kPickArtSize * kPickArtSize * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM));
        ESP_RETURN_ON_FALSE(s_picks[i].pixels != nullptr, ESP_ERR_NO_MEM, TAG, "favourite cover");
    }
    s_still = static_cast<PickCover *>(
        heap_caps_calloc(1, sizeof(PickCover), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_still != nullptr, ESP_ERR_NO_MEM, TAG, "still");
    s_still->pixels = static_cast<std::uint16_t *>(
        heap_caps_malloc(kStillW * kStillH * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(s_still->pixels != nullptr, ESP_ERR_NO_MEM, TAG, "still pixels");

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

void set_still_url(const char *url)
{
    if (s_lock == nullptr || s_still == nullptr) {
        return;
    }
    const char *wanted = url != nullptr ? url : "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = std::strncmp(s_still->wanted, wanted, PATH_SIZE) != 0;
    if (changed) {
        copy_path(s_still->wanted, wanted);
        s_still->retry_at = 0;
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
