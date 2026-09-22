#include "ical_parse.h"

#include "clock_math.h"

#include <cstring>
#include <ctime>

namespace ical {
namespace {

// RFC 5545 breaks any line longer than 75 octets and continues it on the next
// one, marked by a leading space or tab. Nothing can be read until that is put
// back together.
std::size_t unfold(char *body, std::size_t length)
{
    std::size_t out = 0;
    for (std::size_t i = 0; i < length;) {
        if (body[i] == '\r' && i + 2 < length && body[i + 1] == '\n' &&
            (body[i + 2] == ' ' || body[i + 2] == '\t')) {
            i += 3;
            continue;
        }
        if (body[i] == '\n' && i + 1 < length && (body[i + 1] == ' ' || body[i + 1] == '\t')) {
            i += 2;
            continue;
        }
        body[out++] = body[i++];
    }
    return out;
}

bool digits(const char *text, int count, int &out)
{
    out = 0;
    for (int i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        out = out * 10 + (text[i] - '0');
    }
    return true;
}

// YYYYMMDD, or YYYYMMDDTHHMMSS with an optional trailing Z. A value carrying no
// zone is taken as UTC: these feeds write everything in it, and guessing the
// other way would move an event by an hour twice a year.
bool stamp(const char *value, std::size_t length, std::int64_t &out)
{
    std::tm when{};
    int     year = 0;
    int     mon  = 0;
    int     day  = 0;
    if (length < 8 || !digits(value, 4, year) || !digits(value + 4, 2, mon) ||
        !digits(value + 6, 2, day)) {
        return false;
    }
    when.tm_year = year - 1900;
    when.tm_mon  = mon - 1;
    when.tm_mday = day;

    if (length >= 15 && value[8] == 'T') {
        int hour = 0;
        int min  = 0;
        int sec  = 0;
        if (!digits(value + 9, 2, hour) || !digits(value + 11, 2, min) ||
            !digits(value + 13, 2, sec)) {
            return false;
        }
        when.tm_hour = hour;
        when.tm_min  = min;
        when.tm_sec  = sec;
    }
    out = static_cast<std::int64_t>(rtc::utc_seconds(when));
    return true;
}

// Commas, semicolons and backslashes arrive escaped, and a literal \n stands
// for a line break the panel has no room for.
void copy_text(const char *value, std::size_t length, char *out, std::size_t size)
{
    std::size_t written = 0;
    for (std::size_t i = 0; i < length && written + 1 < size; ++i) {
        char c = value[i];
        if (c == '\\' && i + 1 < length) {
            const char next = value[++i];
            c = next == 'n' || next == 'N' ? ' ' : next;
        }
        out[written++] = c;
    }
    out[written] = '\0';
}

bool named(const char *line, std::size_t length, const char *name, const char *&value,
           std::size_t &value_len)
{
    const std::size_t name_len = std::strlen(name);
    if (length <= name_len || std::strncmp(line, name, name_len) != 0) {
        return false;
    }
    // Either NAME:value or NAME;PARAM=...:value, never NAMEX:value.
    std::size_t colon = name_len;
    if (line[name_len] == ';') {
        while (colon < length && line[colon] != ':') {
            ++colon;
        }
    } else if (line[name_len] != ':') {
        return false;
    }
    if (colon >= length || line[colon] != ':') {
        return false;
    }
    value     = line + colon + 1;
    value_len = length - colon - 1;
    return true;
}

}  // namespace

int parse(char *body, std::size_t length, std::uint8_t feed, Event *out, int capacity)
{
    if (body == nullptr || out == nullptr || capacity <= 0) {
        return 0;
    }
    length = unfold(body, length);

    int   stored = 0;
    bool  inside = false;
    Event current{};

    std::size_t at = 0;
    while (at < length && stored < capacity) {
        std::size_t end = at;
        while (end < length && body[end] != '\n') {
            ++end;
        }
        std::size_t line_len = end - at;
        if (line_len > 0 && body[at + line_len - 1] == '\r') {
            --line_len;
        }
        const char *line = body + at;
        at               = end + 1;

        const char *value     = nullptr;
        std::size_t value_len = 0;

        if (line_len == 12 && std::strncmp(line, "BEGIN:VEVENT", 12) == 0) {
            inside  = true;
            current = Event{};
            current.feed = feed;
            continue;
        }
        if (!inside) {
            continue;
        }
        if (line_len == 10 && std::strncmp(line, "END:VEVENT", 10) == 0) {
            inside = false;
            if (current.start != 0) {
                if (current.end < current.start) {
                    current.end = current.start;
                }
                out[stored++] = current;
            }
            continue;
        }

        if (named(line, line_len, "SUMMARY", value, value_len)) {
            copy_text(value, value_len, current.summary, sizeof(current.summary));
        } else if (named(line, line_len, "LOCATION", value, value_len)) {
            copy_text(value, value_len, current.location, sizeof(current.location));
        } else if (named(line, line_len, "DTSTART", value, value_len)) {
            stamp(value, value_len, current.start);
        } else if (named(line, line_len, "DTEND", value, value_len)) {
            stamp(value, value_len, current.end);
        }
    }
    return stored;
}

}  // namespace ical
