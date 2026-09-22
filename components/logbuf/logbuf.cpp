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

namespace logbuf {
namespace {
constexpr char TAG[] = "logbuf";

constexpr int         LINES_PER_CHANNEL = 64;
constexpr std::size_t LINE_BYTES        = 128;
constexpr std::size_t TAG_BYTES         = 32;

struct Line {
    std::time_t  wall;
    std::int64_t uptime_us;
    char         level;
    char         tag[TAG_BYTES];
    char         text[LINE_BYTES];
};

constexpr std::time_t CLOCK_SET_AFTER = 1600000000;

Line        *s_lines    = nullptr;
int          s_channels = 0;
int         *s_next     = nullptr;
int         *s_held     = nullptr;
Router       s_router   = nullptr;
portMUX_TYPE s_lock     = portMUX_INITIALIZER_UNLOCKED;

vprintf_like_t s_next_sink = nullptr;

void split(const char *line, char &level, const char *&tag, std::size_t &tag_len,
           const char *&body)
{
    level   = 'I';
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
    level   = line[0];
    tag     = start;
    tag_len = static_cast<std::size_t>(colon - start);
    body    = colon + 2;
}

void store(const char *line)
{
    char        level   = 'I';
    const char *tag     = nullptr;
    const char *body    = nullptr;
    std::size_t tag_len = 0;
    split(line, level, tag, tag_len, body);

    char named[TAG_BYTES];
    std::memcpy(named, tag, tag_len);
    named[tag_len] = '\0';

    const int channel = s_router(named);
    if (channel < 0 || channel >= s_channels) {
        return;
    }

    const std::time_t  now    = std::time(nullptr);
    const std::int64_t uptime = esp_timer_get_time();

    portENTER_CRITICAL(&s_lock);
    Line &slot     = s_lines[channel * LINES_PER_CHANNEL + s_next[channel]];
    slot.wall      = now >= CLOCK_SET_AFTER ? now : 0;
    slot.uptime_us = uptime;
    slot.level     = level;
    std::memcpy(slot.tag, named, tag_len + 1);
    std::strncpy(slot.text, body, LINE_BYTES - 1);
    slot.text[LINE_BYTES - 1] = '\0';
    s_next[channel]           = (s_next[channel] + 1) % LINES_PER_CHANNEL;
    if (s_held[channel] < LINES_PER_CHANNEL) {
        ++s_held[channel];
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

}  // namespace

esp_err_t start(int channels, Router router)
{
    ESP_RETURN_ON_FALSE(s_lines == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(channels > 0 && router != nullptr, ESP_ERR_INVALID_ARG, TAG, "arguments");

    const auto lines = static_cast<std::size_t>(channels) * LINES_PER_CHANNEL;
    s_lines          = static_cast<Line *>(
        heap_caps_calloc(lines, sizeof(Line), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_next = static_cast<int *>(heap_caps_calloc(static_cast<std::size_t>(channels), sizeof(int),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_held = static_cast<int *>(heap_caps_calloc(static_cast<std::size_t>(channels), sizeof(int),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(s_lines != nullptr && s_next != nullptr && s_held != nullptr,
                        ESP_ERR_NO_MEM, TAG, "rings");

    s_channels  = channels;
    s_router    = router;
    s_next_sink = esp_log_set_vprintf(sink);
    return ESP_OK;
}

int count(int channel)
{
    if (s_lines == nullptr || channel < 0 || channel >= s_channels) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    const int held = s_held[channel];
    portEXIT_CRITICAL(&s_lock);
    return held;
}

bool at(int channel, int index, Entry &out)
{
    if (s_lines == nullptr || channel < 0 || channel >= s_channels || index < 0) {
        return false;
    }

    portENTER_CRITICAL(&s_lock);
    const int held = s_held[channel];
    const int next = s_next[channel];
    portEXIT_CRITICAL(&s_lock);

    if (index >= held) {
        return false;
    }

    const int   age  = held - index;
    const Line &line = s_lines[channel * LINES_PER_CHANNEL +
                               (next - age + LINES_PER_CHANNEL * 2) % LINES_PER_CHANNEL];

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

    out.level = line.level;
    std::snprintf(out.text, sizeof(out.text), "%s  %s: %s", stamp, line.tag, line.text);
    return true;
}

}  // namespace logbuf
