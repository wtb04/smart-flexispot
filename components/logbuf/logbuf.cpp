#include "logbuf.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>

namespace logbuf {
namespace {

constexpr char TAG[] = "logbuf";

// A boot alone writes more than a hundred lines, so a short ring had thrown
// most subsystems' history away before anyone could look at it. This lives in
// PSRAM, where the room is not worth counting.
constexpr int         LINE_COUNT = 512;
constexpr std::size_t LINE_BYTES = 144;
// Long enough for the longest tag in the system. At sixteen, esp_netif_handlers
// and websocket_client were dropped as unparseable and belonged to nothing.
constexpr std::size_t TAG_BYTES  = 32;

struct Line {
    std::time_t  wall;   // 0 until the clock has been set
    std::int64_t uptime_us;
    char         tag[TAG_BYTES];
    char         text[LINE_BYTES];
};

// Before SNTP lands the clock sits near the epoch, and a wall time then is a
// lie; uptime is what there is.
constexpr std::time_t CLOCK_SET_AFTER = 1600000000;

Line        *s_lines = nullptr;
int          s_next  = 0;
int          s_held  = 0;
portMUX_TYPE s_lock  = portMUX_INITIALIZER_UNLOCKED;

vprintf_like_t s_next_sink = nullptr;

// "I (12345) tag: message" is what the logging system emits. Anything that does
// not look like that is kept whole under an empty tag rather than dropped.
void split(const char *line, const char *&tag, std::size_t &tag_len, const char *&body)
{
    tag     = "";
    tag_len = 0;
    body    = line;

    const char *close = std::strstr(line, ") ");
    if (line[0] == '\0' || line[1] != ' ' || line[2] != '(' || close == nullptr) {
        return;
    }
    const char *start = close + 2;
    const char *colon = std::strstr(start, ": ");
    if (colon == nullptr || static_cast<std::size_t>(colon - start) >= TAG_BYTES) {
        return;
    }
    tag     = start;
    tag_len = static_cast<std::size_t>(colon - start);
    body    = colon + 2;
}

void store(const char *line)
{
    const char *tag     = nullptr;
    const char *body    = nullptr;
    std::size_t tag_len = 0;
    split(line, tag, tag_len, body);

    const std::time_t  now    = std::time(nullptr);
    const std::int64_t uptime = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    Line &slot     = s_lines[s_next];
    slot.wall      = now >= CLOCK_SET_AFTER ? now : 0;
    slot.uptime_us = uptime;
    std::memcpy(slot.tag, tag, tag_len);
    slot.tag[tag_len] = '\0';
    std::strncpy(slot.text, body, LINE_BYTES - 1);
    slot.text[LINE_BYTES - 1] = '\0';
    s_next                  = (s_next + 1) % LINE_COUNT;
    if (s_held < LINE_COUNT) {
        ++s_held;
    }
    portEXIT_CRITICAL(&s_lock);
}

int sink(const char *format, va_list args)
{
    if (s_lines != nullptr && !xPortInIsrContext()) {
        va_list copy;
        va_copy(copy, args);
        char line[LINE_BYTES];
        vsnprintf(line, sizeof(line), format, copy);
        va_end(copy);

        // The logging system ends every line itself; keeping it would double
        // the spacing wherever these are shown.
        char *end = line + std::strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r')) {
            *--end = '\0';
        }
        if (line[0] != '\0') {
            store(line);
        }
    }
    return s_next_sink != nullptr ? s_next_sink(format, args) : 0;
}

bool matches(const char *tag, const char *const *tags, int tag_count)
{
    for (int i = 0; i < tag_count; ++i) {
        if (std::strcmp(tag, tags[i]) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

esp_err_t start()
{
    ESP_RETURN_ON_FALSE(s_lines == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_lines = static_cast<Line *>(
        heap_caps_calloc(LINE_COUNT, sizeof(Line), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_lines != nullptr, ESP_ERR_NO_MEM, TAG, "ring");

    s_next_sink = esp_log_set_vprintf(sink);
    return ESP_OK;
}

int recent(const char *const *tags, int tag_count, char *out, std::size_t out_size, int max_lines)
{
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (s_lines == nullptr) {
        return 0;
    }

    // Newest first to find which lines to keep, then written oldest first.
    int  wanted[64];
    int  found = 0;
    const int cap = max_lines < static_cast<int>(std::size(wanted))
                        ? max_lines
                        : static_cast<int>(std::size(wanted));

    portENTER_CRITICAL(&s_lock);
    const int held = s_held;
    const int next = s_next;
    portEXIT_CRITICAL(&s_lock);

    for (int age = 1; age <= held && found < cap; ++age) {
        const int index = (next - age + LINE_COUNT * 2) % LINE_COUNT;
        if (matches(s_lines[index].tag, tags, tag_count)) {
            wanted[found++] = index;
        }
    }

    std::size_t used = 0;
    for (int i = found - 1; i >= 0; --i) {
        const Line &line = s_lines[wanted[i]];

        char stamp[16];
        if (line.wall != 0) {
            std::tm local{};
            localtime_r(&line.wall, &local);
            std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", local.tm_hour, local.tm_min,
                          local.tm_sec);
        } else {
            std::snprintf(stamp, sizeof(stamp), "+%5.1fs",
                          static_cast<double>(line.uptime_us) / 1000000.0);
        }

        char joined[LINE_BYTES + 48];
        const int length = std::snprintf(joined, sizeof(joined), "%s%s  %s: %s",
                                         used > 0 ? "\n" : "", stamp, line.tag, line.text);
        if (length <= 0 || used + static_cast<std::size_t>(length) >= out_size) {
            break;
        }
        std::memcpy(out + used, joined, static_cast<std::size_t>(length));
        used += static_cast<std::size_t>(length);
        out[used] = '\0';
    }
    return found;
}

}  // namespace logbuf
