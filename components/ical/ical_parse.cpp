#include "ical_parse.h"

#include "clock_math.h"

#include <cstring>
#include <ctime>

namespace ical {
namespace {
// A line break and the space or tab that marks the next line as a continuation.
constexpr std::size_t CRLF_FOLD_LENGTH = 3;
constexpr std::size_t LF_FOLD_LENGTH   = 2;

// Where each part sits in YYYYMMDDTHHMMSSZ.
constexpr int         YEAR_DIGITS      = 4;
constexpr int         FIELD_DIGITS     = 2;
constexpr std::size_t MONTH_AT         = 4;
constexpr std::size_t DAY_AT           = 6;
constexpr std::size_t TIME_MARK_AT     = 8;
constexpr std::size_t HOUR_AT          = 9;
constexpr std::size_t MINUTE_AT        = 11;
constexpr std::size_t SECOND_AT        = 13;
constexpr std::size_t UTC_MARK_AT      = 15;
constexpr std::size_t DATE_LENGTH      = sizeof("YYYYMMDD") - 1;
constexpr std::size_t DATE_TIME_LENGTH = sizeof("YYYYMMDDTHHMMSS") - 1;

constexpr int TM_YEAR_BASE = 1900;

constexpr char        ZONE_PARAM[]   = ";TZID=";
constexpr std::size_t ZONE_PARAM_LEN = sizeof(ZONE_PARAM) - 1;

constexpr char BEGIN_EVENT[] = "BEGIN:VEVENT";
constexpr char END_EVENT[]   = "END:VEVENT";

bool continues(char c)
{
    return c == ' ' || c == '\t';
}

// RFC 5545 breaks any line longer than 75 octets and continues it on the next
// one, marked by a leading space or tab. Nothing can be read until that is put
// back together.
std::size_t unfold(char *body, std::size_t length)
{
    std::size_t out = 0;
    for (std::size_t i = 0; i < length;) {
        if (body[i] == '\r' && i + CRLF_FOLD_LENGTH <= length && body[i + 1] == '\n' &&
            continues(body[i + 2])) {
            i += CRLF_FOLD_LENGTH;
            continue;
        }
        if (body[i] == '\n' && i + LF_FOLD_LENGTH <= length && continues(body[i + 1])) {
            i += LF_FOLD_LENGTH;
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
// zone is taken as UTC: the timetable feeds write everything in it, and guessing
// the other way would move an event by an hour twice a year. One in a named zone
// (TZID=...) is wall-clock time in the panel's own zone: Outlook calls it
// "Customized Time Zone", but it is this one.
bool stamp(const char *value, std::size_t length, std::int64_t &out, bool zoned = false)
{
    std::tm when{};
    int     year = 0;
    int     mon  = 0;
    int     day  = 0;
    if (length < DATE_LENGTH || !digits(value, YEAR_DIGITS, year) ||
        !digits(value + MONTH_AT, FIELD_DIGITS, mon) ||
        !digits(value + DAY_AT, FIELD_DIGITS, day)) {
        return false;
    }
    when.tm_year = year - TM_YEAR_BASE;
    when.tm_mon  = mon - 1;
    when.tm_mday = day;

    if (length >= DATE_TIME_LENGTH && value[TIME_MARK_AT] == 'T') {
        int hour = 0;
        int min  = 0;
        int sec  = 0;
        if (!digits(value + HOUR_AT, FIELD_DIGITS, hour) ||
            !digits(value + MINUTE_AT, FIELD_DIGITS, min) ||
            !digits(value + SECOND_AT, FIELD_DIGITS, sec)) {
            return false;
        }
        when.tm_hour = hour;
        when.tm_min  = min;
        when.tm_sec  = sec;
    }
    const bool utc = length > UTC_MARK_AT && value[UTC_MARK_AT] == 'Z';
    if (zoned && !utc) {
        when.tm_isdst = -1;
        out           = static_cast<std::int64_t>(std::mktime(&when));
        return true;
    }
    out = static_cast<std::int64_t>(rtc::utc_seconds(when));
    return true;
}

bool zoned(const char *line, const char *value)
{
    for (const char *at = line; at + (ZONE_PARAM_LEN - 1) <= value; ++at) {
        if (std::strncmp(at, ZONE_PARAM, ZONE_PARAM_LEN) == 0) {
            return true;
        }
    }
    return false;
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

template <std::size_t N>
bool line_is(const char *line, std::size_t length, const char (&word)[N])
{
    return length == N - 1 && std::strncmp(line, word, N - 1) == 0;
}

/** The line starting at `at`, without its line break; `at` moves to the next. */
const char *next_line(const char *body, std::size_t length, std::size_t &at,
                      std::size_t &line_len)
{
    std::size_t end = at;
    while (end < length && body[end] != '\n') {
        ++end;
    }
    line_len = end - at;
    if (line_len > 0 && body[at + line_len - 1] == '\r') {
        --line_len;
    }
    const char *line = body + at;
    at               = end + 1;
    return line;
}

void read_property(const char *line, std::size_t line_len, Event &event)
{
    const char *value     = nullptr;
    std::size_t value_len = 0;
    if (named(line, line_len, "SUMMARY", value, value_len)) {
        copy_text(value, value_len, event.summary, sizeof(event.summary));
    } else if (named(line, line_len, "LOCATION", value, value_len)) {
        copy_text(value, value_len, event.location, sizeof(event.location));
    } else if (named(line, line_len, "DTSTART", value, value_len)) {
        stamp(value, value_len, event.start, zoned(line, value));
    } else if (named(line, line_len, "DTEND", value, value_len)) {
        stamp(value, value_len, event.end, zoned(line, value));
    }
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
        std::size_t       line_len = 0;
        const char *const line     = next_line(body, length, at, line_len);

        if (line_is(line, line_len, BEGIN_EVENT)) {
            inside       = true;
            current      = Event{};
            current.feed = feed;
            continue;
        }
        if (!inside) {
            continue;
        }
        if (line_is(line, line_len, END_EVENT)) {
            inside = false;
            if (current.start != 0) {
                if (current.end < current.start) {
                    current.end = current.start;
                }
                out[stored++] = current;
            }
            continue;
        }
        read_property(line, line_len, current);
    }
    return stored;
}

}  // namespace ical
