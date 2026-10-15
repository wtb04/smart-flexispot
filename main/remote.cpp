#include "remote.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_private/freertos_debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "last_words.h"
#include "board.h"
#include "desk.h"
#include "logbuf.h"
#include "app_state.h"
#include "jobs.h"
#include "net_stream.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "ota.h"
#include "power.h"
#include "kit.h"
#include "lab.h"
#include "ui.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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
    std::size_t listed = LISTED_BLOCK;
};

bool walk_block(walker_heap_into_t, walker_block_info_t block, void *user)
{
    auto *walk = static_cast<Walk *>(user);
    if (!block.used) {
        return true;
    }
    if (block.size < walk->listed || walk->count == MAX_BLOCKS) {
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
    // ?psram: PSRAM's big blocks rather than the DMA-capable heap's
    char query[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    const bool     psram = std::strcmp(query, "psram") == 0;
    const uint32_t caps  = psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DMA;
    Walk walk{blocks, 0, 0, 0, psram ? 32 * 1024 : LISTED_BLOCK};
    heap_caps_walk(caps, walk_block, &walk);

    httpd_resp_set_type(req, "text/plain");
    char line[128];
    int  n = std::snprintf(line, sizeof(line), "free %u largest %u low %u\n",
                           static_cast<unsigned>(heap_caps_get_free_size(caps)),
                           static_cast<unsigned>(heap_caps_get_largest_free_block(caps)),
                           static_cast<unsigned>(heap_caps_get_minimum_free_size(caps)));
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

constexpr std::uint32_t PANEL_LOCK_MS = 1000;

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
    constexpr std::int64_t SLOW_FRAME_US = 150 * units::kUsPerMs;  // full-screen frames take 90: only the really slow
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

// The sockets open now, a line each: kind, local port and whom it talks to.
// lwIP has 24 in all; run out, and nothing connects, the web server included.
int list_sockets(char *out, std::size_t size)
{
    int n = 0, open = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
        sockaddr_storage local{}, peer{};
        socklen_t        len = sizeof local;
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&local), &len) != 0) {
            continue;
        }
        ++open;
        int       type = 0;
        socklen_t tl   = sizeof type;
        getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl);
        len = sizeof peer;
        const bool linked = getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &len) == 0;
        // the server's sockets are IPv6 with the IPv4 peer in the address's last four bytes
        const auto split = [](const sockaddr_storage &a, std::uint32_t &ip4, unsigned &port) {
            if (a.ss_family == AF_INET6) {
                const auto &v6 = reinterpret_cast<const sockaddr_in6 &>(a);
                std::memcpy(&ip4, &v6.sin6_addr.s6_addr[12], 4);
                port = ntohs(v6.sin6_port);
            } else {
                const auto &v4 = reinterpret_cast<const sockaddr_in &>(a);
                ip4  = v4.sin_addr.s_addr;
                port = ntohs(v4.sin_port);
            }
        };
        std::uint32_t lip = 0, pip = 0;
        unsigned      lport = 0, pport = 0;
        split(local, lip, lport);
        char to[32] = "-";
        if (linked) {
            split(peer, pip, pport);
            const auto *b4 = reinterpret_cast<const std::uint8_t *>(&pip);
            std::snprintf(to, sizeof to, "%u.%u.%u.%u:%u", b4[0], b4[1], b4[2], b4[3], pport);
        }
        if (out != nullptr && n < static_cast<int>(size) - 80) {
            n += std::snprintf(out + n, size - n, "%2d %s :%u -> %s\n", fd, type == SOCK_STREAM ? "tcp" : "udp", lport, to);
        }
    }
    if (out != nullptr) {
        n += std::snprintf(out + n, size - n, "%d of %d open\n", open, CONFIG_LWIP_MAX_SOCKETS);
    }
    return out != nullptr ? n : open;
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
        // Two lines, as the log keeps only so much of each. The largest block
        // is found by walking the heap with interrupts off, which took 5.5 ms
        // across PSRAM and cost the panel a frame: only for the small heaps.
        ESP_LOGI(TAG, "heap: int %u/%u/%u dma %u/%u psram %u KB",
                 kib(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 kib(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 kib(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                 kib(heap_caps_get_free_size(MALLOC_CAP_DMA)), kib(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                 kib(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        ESP_LOGI(TAG, "lvgl: %u obj %u timers %u tasks, %u frames, slowest %d ms", static_cast<unsigned>(objects),
                 static_cast<unsigned>(timers), static_cast<unsigned>(uxTaskGetNumberOfTasks()),
                 static_cast<unsigned>(s_frames.exchange(0)),
                 static_cast<int>(s_slowest_us.exchange(0) / units::kUsPerMs));
        static std::uint32_t refreshed = 0;
        const std::uint32_t  now       = board::refreshes();
        ESP_LOGI(TAG, "panel: %u refreshes, %d sockets", static_cast<unsigned>(now - refreshed),
                 list_sockets(nullptr, 0));
        refreshed = now;
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
// Every live connection net keeps, and how it is doing; ?restart=N begins
// that one again, as a drop would.
esp_err_t streams_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    char query[QUERY_MAX] = "", value[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "restart", value, sizeof(value)) == ESP_OK) {
        net::stream_restart(std::atoi(value));
    }
    httpd_resp_set_type(req, "text/plain");
    for (int i = 0; i < net::stream_count(); ++i) {
        const net::StreamStatus status = net::stream_status(i);
        char line[160];
        std::snprintf(line, sizeof(line), "%d %-9s %-10s for %d s, next in %d s, ready %lu times, dropped %lu%s%s\n", i,
                      net::stream_name(i), net::stream_state_name(status.state), status.for_ms / 1000,
                      status.next_in_ms / 1000, static_cast<unsigned long>(status.readies),
                      static_cast<unsigned long>(status.drops), status.last_error.empty() ? "" : ", last: ",
                      status.last_error.c_str());
        httpd_resp_sendstr_chunk(req, line);
    }
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// Every job the shared workers run, and how each is doing; ?poke=N runs one
// now, ?screen=0 or 1 tells everything listening that the screen went dark or
// lit, without the backlight or Home Assistant hearing of it, and ?stick=S has
// a quick job hang for S seconds, for the watchdog to find.
esp_err_t jobs_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    char query[QUERY_MAX] = "", value[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "poke", value, sizeof(value)) == ESP_OK) {
        jobs::poke(std::atoi(value));
    }
    if (httpd_query_key_value(query, "screen", value, sizeof(value)) == ESP_OK) {
        app::set(app::Fact::ScreenOn, std::atoi(value) != 0);
    }
    if (httpd_query_key_value(query, "stick", value, sizeof(value)) == ESP_OK) {
        static int stick_s = 0;
        stick_s            = std::clamp(std::atoi(value), 1, 60);
        jobs::Spec spec;
        spec.name     = "stuck on purpose";
        spec.first_ms = 0;
        spec.run      = [] {
            vTaskDelay(pdMS_TO_TICKS(stick_s * 1000));
            return jobs::sleep();
        };
        jobs::add(std::move(spec));
    }
    httpd_resp_set_type(req, "text/plain");
    for (int i = 0; i < jobs::count(); ++i) {
        const jobs::Status status = jobs::status(i);
        char               due[16];
        std::snprintf(due, sizeof(due), status.due_in_ms < 0 ? "when poked" : "in %d ms", status.due_in_ms);
        char line[160];
        std::snprintf(line, sizeof(line), "%d %-12s %-8s next %-12s ran %lu times, last %d ms, most %d ms%s\n", i,
                      jobs::name(i), status.running ? "running" : status.waiting ? "waiting" : "idle", due,
                      static_cast<unsigned long>(status.runs), status.last_ms, status.most_ms,
                      status.failures > 0 ? ", failing" : "");
        httpd_resp_sendstr_chunk(req, line);
    }
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// The last crash, whole, as the panic handler wrote it: for
// `idf.py coredump-info -c dump.elf` with this build's ELF. ?erase=1 clears it.
esp_err_t coredump_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    char query[QUERY_MAX] = "", value[8] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "erase", value, sizeof(value)) == ESP_OK) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, esp_core_dump_image_erase() == ESP_OK ? "erased\n" : "nothing to erase\n");
    }
    std::size_t at = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&at, &size) != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "no crash dump kept\n");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    static char chunk[4096];
    for (std::size_t sent = 0; sent < size;) {
        const std::size_t n = std::min(sizeof(chunk), size - sent);
        if (esp_flash_read(nullptr, chunk, at + sent, n) != ESP_OK ||
            httpd_resp_send_chunk(req, chunk, static_cast<ssize_t>(n)) != ESP_OK) {
            return ESP_FAIL;
        }
        sent += n;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// Why this run started and, after one nobody asked for, what the run before
// left behind: the log's copy is soon pushed out.
esp_err_t restart_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    const std::time_t started = std::time(nullptr) - esp_timer_get_time() / units::kUsPerSecond;
    std::tm           local{};
    localtime_r(&started, &local);
    char head[48];
    std::snprintf(head, sizeof(head), "up since %02d-%02d %02d:%02d\n", local.tm_mday, local.tm_mon + 1,
                  local.tm_hour, local.tm_min);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr_chunk(req, head);
    httpd_resp_sendstr_chunk(req, last_words::report());
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// The desk's trace, of what it heard and was asked; ?clear starts it over,
// ?tap=N taps preset N (1 to 6) as a finger on its button would.
esp_err_t desk_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    constexpr std::size_t TEXT_SIZE = 16 * 1024;  // 400 lines of 36
    static char *text = static_cast<char *>(heap_caps_malloc(TEXT_SIZE, MALLOC_CAP_SPIRAM));  // internal RAM is short
    if (text == nullptr) {
        return httpd_resp_sendstr(req, "no room\n");
    }
    char query[24] = "";
    char value[8]  = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    int n = 0;
    if (httpd_query_key_value(query, "tap", value, sizeof(value)) == ESP_OK) {
        const int preset = std::atoi(value);
        if (preset < 1 || preset > ui::kPresetCount) {
            n = std::snprintf(text, TEXT_SIZE, "no such preset\n");
        } else if (!lvgl_port_lock(PANEL_LOCK_MS)) {
            n = std::snprintf(text, TEXT_SIZE, "screen busy\n");
        } else {
            ui::desk_tap(preset - 1);
            lvgl_port_unlock();
            n = std::snprintf(text, TEXT_SIZE, "tapped preset %d\n", preset);
        }
    } else {
        n = desk::trace_text(text, TEXT_SIZE, std::strcmp(query, "clear") == 0);
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, n);
}

// How long the radar takes to open its map, draw, zoom and close. With
// ?open it is only shown, so its feed can start before the bench.
esp_err_t frozen_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    const lv_draw_buf_t *pic = ui::kit::frozen_picture();
    if (pic == nullptr) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "no frozen picture yet\n");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char head[32];
    const int n = std::snprintf(head, sizeof(head), "RGB565 %d %d\n", static_cast<int>(pic->header.w),
                                static_cast<int>(pic->header.h));
    esp_err_t err = httpd_resp_send_chunk(req, head, n);
    for (std::uint32_t y = 0; err == ESP_OK && y < pic->header.h; ++y) {
        err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(pic->data + y * pic->header.stride),
                                    static_cast<ssize_t>(pic->header.w * 2));
    }
    return err == ESP_OK ? httpd_resp_send_chunk(req, nullptr, 0) : err;
}

// What the panel shows, straight from its frame buffer, unturned: a picture
// taken by LVGL drawing the screen again misses whatever was drawn past it.
esp_err_t panel_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    int w = 0, h = 0;
    const std::uint16_t *fb = board::shown_frame(&w, &h);
    if (fb == nullptr) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "no panel\n");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    char head[32];
    const int n = std::snprintf(head, sizeof(head), "RGB565 %d %d\n", w, h);
    esp_err_t err = httpd_resp_send_chunk(req, head, n);
    constexpr int ROWS = 8;
    for (int y = 0; err == ESP_OK && y < h; y += ROWS) {
        err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(fb + y * w),
                                    static_cast<ssize_t>(std::min(ROWS, h - y) * w * 2));
    }
    board::let_go_of_frame();
    return err == ESP_OK ? httpd_resp_send_chunk(req, nullptr, 0) : err;
}

// A finger on the screen from here, for testing on the panel with nobody at
// it: the touch's own reading, overridden while it lasts, so what LVGL sees is
// what a real press is.
struct Touch {
    lv_indev_t      *indev = nullptr;
    lv_indev_read_cb_t read = nullptr;
    lv_point_t       raw{};
    std::uint32_t    until = 0;
    bool             on    = false, lifting = false;
} s_touch;

lv_point_t turned(lv_point_t p)
{
    if (lv_indev_get_ccw(s_touch.indev)) {
        lv_display_rotate_point_ccw(lv_indev_get_display(s_touch.indev), &p);
    } else {
        lv_display_rotate_point(lv_indev_get_display(s_touch.indev), &p);
    }
    return p;
}

void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_touch.read(indev, data);
    if (!s_touch.on) {
        return;
    }
    data->point = s_touch.raw;
    if (static_cast<std::int32_t>(lv_tick_get() - s_touch.until) < 0) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_touch.on  = false;
    }
}

bool press(int x, int y, int ms)
{
    if (s_touch.indev == nullptr) {
        for (lv_indev_t *dev = lv_indev_get_next(nullptr); dev != nullptr; dev = lv_indev_get_next(dev)) {
            if (lv_indev_get_type(dev) == LV_INDEV_TYPE_POINTER) {
                s_touch.indev = dev;
                s_touch.read  = lv_indev_get_read_cb(dev);
                lv_indev_set_read_cb(dev, touch_read);
                break;
            }
        }
        if (s_touch.indev == nullptr) {
            return false;
        }
    }
    // the screen's point back to the touch's own, from where the turn sends three
    const lv_point_t o = turned({0, 0}), ex = turned({1, 0}), ey = turned({0, 1});
    const int ax = ex.x - o.x, ay = ex.y - o.y, bx = ey.x - o.x, by = ey.y - o.y;
    const int dx = x - o.x, dy = y - o.y;
    s_touch.raw   = {static_cast<std::int32_t>(ax * dx + ay * dy), static_cast<std::int32_t>(bx * dx + by * dy)};
    s_touch.until = lv_tick_get() + static_cast<std::uint32_t>(ms);
    s_touch.on    = true;
    lv_timer_create(
        [](lv_timer_t *t) {
            lv_indev_read(s_touch.indev);  // a touch that only reads when the controller signals
            if (!s_touch.on) {
                lv_timer_delete(t);
            }
        },
        10, nullptr);
    return true;
}

#if LV_USE_PROFILER
// LVGL's own steps, each added up as it ends: how long it took itself and with
// what it called, and how often, per task. A profiling build's alone (build/prof).
namespace prof {
struct Step {
    const char  *name;
    std::int64_t own_us, all_us;
    int          calls;
};
struct Open {
    int          step;
    std::int64_t at_us, inner_us;
};
constexpr int STEPS = 160, DEPTH = 48, TASKS = 8;
struct Task {
    void *handle;
    Open  open[DEPTH];
    int   depth;
};
EXT_RAM_BSS_ATTR Step s_steps[STEPS];
EXT_RAM_BSS_ATTR Task s_tasks[TASKS];
int                   s_step_count = 0, s_task_count = 0;
std::int64_t          s_started_us = 0;
std::atomic<bool>     s_on{false};
portMUX_TYPE          s_lock = portMUX_INITIALIZER_UNLOCKED;

Task *task_of(void *handle)
{
    for (int i = 0; i < s_task_count; ++i) {
        if (s_tasks[i].handle == handle) {
            return &s_tasks[i];
        }
    }
    if (s_task_count == TASKS) {
        return nullptr;
    }
    Task &t = s_tasks[s_task_count++];
    t.handle = handle;
    t.depth  = 0;
    return &t;
}

int step_of(const char *name)
{
    for (int i = 0; i < s_step_count; ++i) {
        if (s_steps[i].name == name) {
            return i;
        }
    }
    if (s_step_count == STEPS) {
        return -1;
    }
    s_steps[s_step_count] = {name, 0, 0, 0};
    return s_step_count++;
}

void start()
{
    portENTER_CRITICAL(&s_lock);
    s_step_count = 0;
    s_task_count = 0;
    s_started_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    s_on.store(true);
}

int report(char *out, std::size_t size)
{
    s_on.store(false);
    vTaskDelay(pdMS_TO_TICKS(20));  // any step under way ends unseen
    std::sort(s_steps, s_steps + s_step_count, [](const Step &a, const Step &b) { return a.own_us > b.own_us; });
    int n = std::snprintf(out, size, "over %.0f ms\n%-44s %9s %9s %7s\n", (esp_timer_get_time() - s_started_us) / 1000.0,
                          "step", "own ms", "all ms", "calls");
    for (int i = 0; i < s_step_count && n < static_cast<int>(size) - 100; ++i) {
        const Step &s = s_steps[i];
        n += std::snprintf(out + n, size - n, "%-44.44s %9.1f %9.1f %7d\n", s.name, s.own_us / 1000.0, s.all_us / 1000.0, s.calls);
    }
    return n;
}
}  // namespace prof

extern "C" void draw_profile_begin(const char *step)
{
    if (!prof::s_on.load(std::memory_order_relaxed)) {
        return;
    }
    const std::int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&prof::s_lock);
    prof::Task *t = prof::task_of(xTaskGetCurrentTaskHandle());
    if (t != nullptr && t->depth < prof::DEPTH) {
        t->open[t->depth++] = {prof::step_of(step), now, 0};
    }
    portEXIT_CRITICAL(&prof::s_lock);
}

extern "C" void draw_profile_end(const char *)
{
    if (!prof::s_on.load(std::memory_order_relaxed)) {
        return;
    }
    const std::int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&prof::s_lock);
    prof::Task *t = prof::task_of(xTaskGetCurrentTaskHandle());
    if (t != nullptr && t->depth > 0) {
        const prof::Open o   = t->open[--t->depth];
        const std::int64_t all = now - o.at_us;
        if (o.step >= 0) {
            prof::s_steps[o.step].all_us += all;
            prof::s_steps[o.step].own_us += all - o.inner_us;
            prof::s_steps[o.step].calls += 1;
        }
        if (t->depth > 0) {
            t->open[t->depth - 1].inner_us += all;
        }
    }
    portEXIT_CRITICAL(&prof::s_lock);
}
#endif

esp_err_t bench_page(httpd_req_t *req)
{
    if (!ota::authorised(req)) {
        return refuse(req);
    }
    EXT_RAM_BSS_ATTR static char text[28000];
    int         n = 0;
    if (lvgl_port_lock(STATS_LOCK_MS)) {
        char query[24] = "";
        httpd_req_get_url_query_str(req, query, sizeof(query));
        if (std::strcmp(query, "lab") == 0) {
            if (!ui::lab::built()) {
                ui::lab::build([]() { return static_cast<std::uint64_t>(esp_timer_get_time()); });
            }
            ui::lab::bench();
            n = std::snprintf(text, sizeof(text), "the lab is up and its bench running; /bench?report in a minute\n");
        } else if (query[0] == 'k' && query[1] == '=' && query[2] != '\0' && ui::lab::built()) {
            ui::lab::key(query[2]);  // a key as the simulator would pass it, to drive the lab from here
            n = std::snprintf(text, sizeof(text), "pressed %c\n", query[2]);
        } else if (query[0] == 'b' && query[1] == '=') {  // what lies behind a popover, to compare them on the panel
            ui::kit::set_behind(query[2] == 'd' ? ui::kit::Behind::Dim
                                  : query[2] == 's' ? ui::kit::Behind::Soft
                                                    : ui::kit::Behind::Blur);
            n = std::snprintf(text, sizeof(text), "backdrop %c\n", query[2]);
        } else if (std::strcmp(query, "socks") == 0) {  // every socket open, to find one that leaks
            n = list_sockets(text, sizeof(text));
        } else if (std::strcmp(query, "fp") == 0 || std::strcmp(query, "fpc") == 0) {  // each frame and touch lately; fpc starts over
            n = board::frame_report(text, sizeof(text), query[2] == 'c');
#if LV_USE_PROFILER
        } else if (std::strcmp(query, "prof") == 0) {  // LVGL's draw steps traced from now
            prof::start();
            n = std::snprintf(text, sizeof(text), "tracing\n");
        } else if (std::strcmp(query, "profd") == 0) {  // and what they cost since
            n = prof::report(text, sizeof(text));
#endif
        } else if (std::strncmp(query, "pm=", 3) == 0) {  // pm=g, r or a: popovers grow, rise or appear
            ui::kit::set_pop_motion(query[3] == 'g' ? ui::kit::PopMotion::Grow
                                    : query[3] == 'r' ? ui::kit::PopMotion::Rise
                                                      : ui::kit::PopMotion::Appear);
            n = std::snprintf(text, sizeof(text), "popovers %c\n", query[3]);
        } else if (std::strncmp(query, "fx=", 3) == 0) {  // fx=0 or 1: the shadows, to feel the panel without them
            ui::kit::set_effects(query[3] != '0');
            n = std::snprintf(text, sizeof(text), "shadows %s\n", query[3] != '0' ? "on" : "off");
        } else if (query[0] == 't' && query[1] == '=') {  // t=x,y,ms: a press there, that long
            int x = 0, y = 0, ms = 80;
            std::sscanf(query + 2, "%d,%d,%d", &x, &y, &ms);
            n = std::snprintf(text, sizeof(text), press(x, y, ms) ? "pressed %d,%d for %d ms\n" : "no touch\n", x, y, ms);
        } else if (std::strcmp(query, "kr") == 0) {  // the design kit's last page change and popover
            n = std::snprintf(text, sizeof(text), "%s\n%s", ui::kit::change_report(), ui::kit::freeze_report());
        } else if (std::strcmp(query, "report") == 0) {
            n = ui::lab::report(text, sizeof(text));
        } else
        n = std::strcmp(query, "open") == 0     ? ui::bench_radar_open(text, sizeof(text))
            : std::strcmp(query, "map") == 0    ? ui::bench_radar_map(text, sizeof(text), true)
            : std::strcmp(query, "back") == 0   ? ui::bench_radar_map(text, sizeof(text), false)
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
        {.uri = "/frozen", .method = HTTP_GET, .handler = frozen_page, .user_ctx = nullptr},
        {.uri = "/panel", .method = HTTP_GET, .handler = panel_page, .user_ctx = nullptr},
        {.uri = "/desk", .method = HTTP_GET, .handler = desk_page, .user_ctx = nullptr},
        {.uri = "/restart", .method = HTTP_GET, .handler = restart_page, .user_ctx = nullptr},
        {.uri = "/streams", .method = HTTP_GET, .handler = streams_page, .user_ctx = nullptr},
        {.uri = "/jobs", .method = HTTP_GET, .handler = jobs_page, .user_ctx = nullptr},
        {.uri = "/coredump", .method = HTTP_GET, .handler = coredump_page, .user_ctx = nullptr},
    };
    for (const httpd_uri_t &page : pages) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &page), TAG, "page");
    }
    return ESP_OK;
}
}  // namespace remote
