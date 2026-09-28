#include "remote.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_private/freertos_debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "ota.h"
#include "power.h"
#include "ui.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef REMOTE_ENABLED
#define REMOTE_ENABLED 0
#endif

namespace remote {
namespace {
constexpr char TAG[] = "remote";

constexpr int           LOG_LINES_DEFAULT = 200;
constexpr int           LOG_LINES_MAX     = 400;
constexpr std::uint32_t ALL_CHANNELS      = 0xffffffff;
constexpr std::size_t   QUERY_MAX         = 32;
constexpr std::size_t   SEND_CHUNK        = 16 * units::kBytesPerKiB;

esp_err_t refuse(httpd_req_t *req)
{
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_sendstr(req, "wrong or missing update key\n");
}

int lines_asked(httpd_req_t *req)
{
    char query[QUERY_MAX] = {};
    char value[QUERY_MAX] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "lines", value, sizeof(value)) == ESP_OK) {
        return std::clamp(std::atoi(value), 1, LOG_LINES_MAX);
    }
    return LOG_LINES_DEFAULT;
}

esp_err_t log_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    const int max     = lines_asked(req);
    auto     *entries = static_cast<logbuf::Entry *>(
        heap_caps_malloc(sizeof(logbuf::Entry) * max, MALLOC_CAP_SPIRAM));
    if (entries == nullptr) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no room\n");
    }
    const int count = logbuf::recent(ALL_CHANNELS, false, entries, max);
    httpd_resp_set_type(req, "text/plain");
    for (int i = 0; i < count; ++i) {
        char line[logbuf::kTextMax + 4];
        const int n = std::snprintf(line, sizeof(line), "%c %s\n", entries[i].level, entries[i].text);
        httpd_resp_send_chunk(req, line, std::min<int>(n, sizeof(line) - 1));
    }
    heap_caps_free(entries);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

constexpr int         MAX_TASKS      = 64;
constexpr int         MAX_BLOCKS     = 512;
constexpr std::size_t LISTED_BLOCK   = 256;  // smaller used blocks are only counted

struct Block {
    void       *ptr;
    std::size_t size;
};

struct Walk {
    Block      *blocks;
    int         count;
    std::size_t small_bytes;
    int         small_count;
};

bool walk_block(walker_heap_into_t, walker_block_info_t block, void *user)
{
    auto *walk = static_cast<Walk *>(user);
    if (!block.used) {
        return true;
    }
    if (block.size < LISTED_BLOCK || walk->count == MAX_BLOCKS) {
        walk->small_bytes += block.size;
        ++walk->small_count;
    } else {
        walk->blocks[walk->count++] = {block.ptr, block.size};
    }
    return true;
}

// The DMA-capable heap: how much is free, then each used block of some size,
// named for the task when it is one's stack.
esp_err_t heap_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    struct Stack {
        const std::uint8_t *start;
        const char         *name;
    };
    auto *stacks = static_cast<Stack *>(heap_caps_calloc(MAX_TASKS, sizeof(Stack), MALLOC_CAP_SPIRAM));
    auto *blocks = static_cast<Block *>(heap_caps_calloc(MAX_BLOCKS, sizeof(Block), MALLOC_CAP_SPIRAM));
    if (stacks == nullptr || blocks == nullptr) {
        heap_caps_free(stacks);
        heap_caps_free(blocks);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no room\n");
    }
    int            tasks = 0;
    TaskIterator_t it{};
    while (tasks < MAX_TASKS && xTaskGetNext(&it) != -1) {
        stacks[tasks++] = {pxTaskGetStackStart(it.pxTaskHandle), pcTaskGetName(it.pxTaskHandle)};
    }
    Walk walk{blocks, 0, 0, 0};
    heap_caps_walk(MALLOC_CAP_DMA, walk_block, &walk);

    httpd_resp_set_type(req, "text/plain");
    char line[128];
    int  n = std::snprintf(line, sizeof(line), "free %u largest %u low %u\n",
                           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                           static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_DMA)));
    httpd_resp_send_chunk(req, line, n);
    for (int i = 0; i < walk.count; ++i) {
        const auto *from = static_cast<const std::uint8_t *>(blocks[i].ptr);
        const char *name = "";
        for (int t = 0; t < tasks; ++t) {
            if (stacks[t].start >= from && stacks[t].start < from + blocks[i].size) {
                name = stacks[t].name;
            }
        }
        n = std::snprintf(line, sizeof(line), "%p %6u %s\n", blocks[i].ptr,
                          static_cast<unsigned>(blocks[i].size), name);
        httpd_resp_send_chunk(req, line, n);
    }
    n = std::snprintf(line, sizeof(line), "smaller: %d blocks, %u bytes\n", walk.small_count,
                      static_cast<unsigned>(walk.small_bytes));
    httpd_resp_send_chunk(req, line, n);
    heap_caps_free(stacks);
    heap_caps_free(blocks);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

constexpr int        POWER_SECONDS_DEFAULT = 20;
constexpr int        POWER_SECONDS_MAX     = 120;
constexpr TickType_t POWER_SAMPLE          = pdMS_TO_TICKS(250);

// The pack's current and voltage averaged over ?seconds=, sampled four times
// a second: what one change costs, measured rather than guessed. On battery
// only; on the cable the charger feeds the panel and the pack reads charging.
esp_err_t power_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    char query[QUERY_MAX] = {};
    char value[QUERY_MAX] = {};
    int  seconds          = POWER_SECONDS_DEFAULT;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
        seconds = std::clamp(std::atoi(value), 1, POWER_SECONDS_MAX);
    }
    double amps = 0.0, volts = 0.0, peak = 0.0;
    int    count = 0, percent = 0;
    const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(seconds * units::kMsPerSecond);
    while (xTaskGetTickCount() < until) {
        power::State state{};
        if (power::read(state) == ESP_OK) {
            amps += state.current_amps;
            volts += state.bus_volts;
            peak    = std::max<double>(peak, state.current_amps);
            percent = state.percent;
            ++count;
        }
        vTaskDelay(POWER_SAMPLE);
    }
    char line[160];
    const double mean_a = count > 0 ? amps / count : 0.0;
    const double mean_v = count > 0 ? volts / count : 0.0;
    const int    n      = std::snprintf(line, sizeof(line),
                                        "%d s, %d samples: %.0f mA mean, %.0f mA peak, %.3f V, %.2f W, %d%%, %d mAh\n",
                                        seconds, count, mean_a * 1000.0, peak * 1000.0, mean_v, mean_a * mean_v,
                                        percent, power::charge_mah());
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, line, n);
}

// A line saying the size, then the pixels as RGB565, row by row.
esp_err_t screen_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    int            width  = 0;
    int            height = 0;
    std::uint16_t *pixels = ui::capture(width, height);
    if (pixels == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "the screen could not be had\n");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char head[32];
    const int n = std::snprintf(head, sizeof(head), "RGB565 %d %d\n", width, height);
    esp_err_t err = httpd_resp_send_chunk(req, head, n);
    const auto *bytes = reinterpret_cast<const char *>(pixels);
    const std::size_t total = static_cast<std::size_t>(width) * height * sizeof(std::uint16_t);
    for (std::size_t at = 0; err == ESP_OK && at < total; at += SEND_CHUNK) {
        err = httpd_resp_send_chunk(req, bytes + at, static_cast<ssize_t>(std::min(SEND_CHUNK, total - at)));
    }
    heap_caps_free(pixels);
    return err == ESP_OK ? httpd_resp_send_chunk(req, nullptr, 0) : err;
}
// Every half minute, in the log: what could run down or pile up over a long
// run, the heaps, the screen's objects and the slowest frame since the last
// line, to see which of them a panel gone slow has run out of.
constexpr std::uint32_t STATS_EVERY_MS  = 30 * units::kMsPerSecond;
constexpr std::uint32_t STATS_LOCK_MS   = 1000;
constexpr std::uint32_t STATS_STACK     = 4096;
constexpr UBaseType_t   STATS_PRIORITY  = 1;

std::atomic<std::int64_t> s_frame_began{0};
std::atomic<std::int64_t> s_slowest_us{0};
std::atomic<std::uint32_t> s_frames{0};

// A frame from REFR_START to REFR_READY, and of that the drawing, from
// RENDER_START to RENDER_READY; the rest is handing it to the panel.
std::atomic<std::int64_t> s_render_began{0};
std::atomic<std::int64_t> s_render_us{0};

void frame_timed(lv_event_t *event)
{
    const std::int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(event)) {
        case LV_EVENT_REFR_START:
            s_frame_began = now;
            s_render_us   = 0;
            return;
        case LV_EVENT_RENDER_START:
            s_render_began = now;
            return;
        case LV_EVENT_RENDER_READY:
            s_render_us += now - s_render_began.load();
            return;
        default:
            break;
    }
    const std::int64_t took = now - s_frame_began.load();
    constexpr std::int64_t SLOW_FRAME_US = 60 * units::kUsPerMs;
    if (took > SLOW_FRAME_US) {
        ESP_LOGI(TAG, "frame %d ms, drawing %d", static_cast<int>(took / units::kUsPerMs),
                 static_cast<int>(s_render_us.load() / units::kUsPerMs));
    }
    if (took > s_slowest_us.load()) {
        s_slowest_us = took;
    }
    ++s_frames;
}

std::uint32_t objects_under(lv_obj_t *obj)
{
    std::uint32_t count = 1;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        count += objects_under(lv_obj_get_child(obj, i));
    }
    return count;
}

[[noreturn]] void stats_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_EVERY_MS));
        std::uint32_t objects = 0, timers = 0;
        if (lvgl_port_lock(STATS_LOCK_MS)) {
            objects = objects_under(lv_screen_active()) + objects_under(lv_layer_top());
            for (lv_timer_t *timer = lv_timer_get_next(nullptr); timer != nullptr; timer = lv_timer_get_next(timer)) {
                ++timers;
            }
            lvgl_port_unlock();
        }
        const auto kib = [](std::size_t bytes) { return static_cast<unsigned>(bytes / units::kBytesPerKiB); };
        // Two lines, as the log keeps only so much of each.
        ESP_LOGI(TAG, "heap: int %u/%u/%u dma %u/%u psram %u/%u KB",
                 kib(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 kib(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 kib(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                 kib(heap_caps_get_free_size(MALLOC_CAP_DMA)), kib(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                 kib(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 kib(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        ESP_LOGI(TAG, "lvgl: %u obj %u timers %u tasks, %u frames, slowest %d ms", static_cast<unsigned>(objects),
                 static_cast<unsigned>(timers), static_cast<unsigned>(uxTaskGetNumberOfTasks()),
                 static_cast<unsigned>(s_frames.exchange(0)),
                 static_cast<int>(s_slowest_us.exchange(0) / units::kUsPerMs));
    }
}

void start_stats()
{
    if (lvgl_port_lock(STATS_LOCK_MS)) {
        lv_display_t *display = lv_display_get_default();
        lv_display_add_event_cb(display, frame_timed, LV_EVENT_REFR_START, nullptr);
        lv_display_add_event_cb(display, frame_timed, LV_EVENT_REFR_READY, nullptr);
        lv_display_add_event_cb(display, frame_timed, LV_EVENT_RENDER_START, nullptr);
        lv_display_add_event_cb(display, frame_timed, LV_EVENT_RENDER_READY, nullptr);
        lvgl_port_unlock();
    }
    xTaskCreate(stats_task, "stats", STATS_STACK, nullptr, STATS_PRIORITY, nullptr);
}
// How long the radar takes to open fullscreen, draw, zoom and close. With
// ?open it is only shown, so its feed can start before the bench.
esp_err_t bench_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    static char text[1024];
    int         n = 0;
    if (lvgl_port_lock(STATS_LOCK_MS)) {
        char query[8] = "";
        httpd_req_get_url_query_str(req, query, sizeof(query));
        n = std::strcmp(query, "open") == 0 ? ui::bench_radar_open(text, sizeof(text))
                                            : ui::bench_radar(text, sizeof(text));
        lvgl_port_unlock();
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, n);
}
}  // namespace

esp_err_t start()
{
    if (!REMOTE_ENABLED) {
        return ESP_OK;
    }
    start_stats();
    httpd_handle_t server = ota::server();
    ESP_RETURN_ON_FALSE(server != nullptr, ESP_ERR_INVALID_STATE, TAG, "no server");
    const httpd_uri_t pages[] = {
        {.uri = "/log", .method = HTTP_GET, .handler = log_page, .user_ctx = nullptr},
        {.uri = "/screen", .method = HTTP_GET, .handler = screen_page, .user_ctx = nullptr},
        {.uri = "/heap", .method = HTTP_GET, .handler = heap_page, .user_ctx = nullptr},
        {.uri = "/power", .method = HTTP_GET, .handler = power_page, .user_ctx = nullptr},
        {.uri = "/bench", .method = HTTP_GET, .handler = bench_page, .user_ctx = nullptr},
    };
    for (const httpd_uri_t &page : pages) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &page), TAG, "page");
    }
    return ESP_OK;
}
}  // namespace remote
