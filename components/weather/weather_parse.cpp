#include "weather_parse.h"

#include "provinces.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

namespace weather {
namespace {
constexpr int ESCAPE_LENGTH = 2;
constexpr int NUMBER_MAX    = 32;

// Like the radar's: over the whole answer, nothing allocated.
struct Scanner {
    const char *p;
    const char *end;

    void skip_space()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            ++p;
        }
    }

    bool take(char c)
    {
        skip_space();
        if (p < end && *p == c) {
            ++p;
            return true;
        }
        return false;
    }


    bool string(const char *&start, std::size_t &length)
    {
        skip_space();
        if (p >= end || *p != '"') {
            return false;
        }
        start = ++p;
        while (p < end) {
            if (*p == '\\') {
                p += ESCAPE_LENGTH;
                continue;
            }
            if (*p == '"') {
                length = static_cast<std::size_t>(p - start);
                ++p;
                return true;
            }
            ++p;
        }
        return false;
    }

    // A number, or NaN for JSON's null, which Open-Meteo gives for an hour a
    // model does not reach.
    bool number(double &out)
    {
        skip_space();
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) {
            p += 4;
            out = NAN;
            return true;
        }
        char text[NUMBER_MAX];
        int  n = 0;
        while (p + n < end && n < NUMBER_MAX - 1 &&
               (std::strchr("+-.eE", p[n]) != nullptr || (p[n] >= '0' && p[n] <= '9'))) {
            text[n] = p[n];
            ++n;
        }
        text[n]    = '\0';
        char *stop = nullptr;
        out        = std::strtod(text, &stop);
        if (stop == text) {
            return false;
        }
        p += stop - text;
        return true;
    }

    bool skip_value()
    {
        skip_space();
        if (p >= end) {
            return false;
        }
        if (*p == '"') {
            const char *start  = nullptr;
            std::size_t length = 0;
            return string(start, length);
        }
        if (*p == '{' || *p == '[') {
            int depth = 0;
            while (p < end) {
                if (*p == '"') {
                    const char *start  = nullptr;
                    std::size_t length = 0;
                    if (!string(start, length)) {
                        return false;
                    }
                    continue;
                }
                if (*p == '{' || *p == '[') {
                    ++depth;
                } else if (*p == '}' || *p == ']') {
                    if (--depth == 0) {
                        ++p;
                        return true;
                    }
                }
                ++p;
            }
            return false;
        }
        while (p < end && *p != ',' && *p != '}' && *p != ']') {
            ++p;
        }
        return true;
    }

    bool key(const char *&start, std::size_t &length)
    {
        return string(start, length) && take(':');
    }

    /** Calls `each(key, length)` with the scanner at each member's value,
     *  which `each` reads or leaves to be skipped by returning false. */
    template <typename Each>
    bool members(Each each)
    {
        if (!take('{')) {
            return false;
        }
        if (take('}')) {
            return true;
        }
        do {
            const char *name   = nullptr;
            std::size_t length = 0;
            if (!key(name, length)) {
                return false;
            }
            const char *before = p;
            if (!each(name, length)) {
                p = before;
                if (!skip_value()) {
                    return false;
                }
            }
        } while (take(','));
        return take('}');
    }

    /** Each number of an array, with its index. */
    template <typename Each>
    bool numbers(Each each)
    {
        if (!take('[')) {
            return false;
        }
        if (take(']')) {
            return true;
        }
        int i = 0;
        do {
            double value = 0;
            if (!number(value)) {
                return false;
            }
            each(i++, value);
        } while (take(','));
        return take(']');
    }
};

bool is(const char *start, std::size_t length, const char *name)
{
    return std::strlen(name) == length && std::strncmp(start, name, length) == 0;
}

float as_float(double value, float otherwise = 0.0f)
{
    return std::isnan(value) ? otherwise : static_cast<float>(value);
}

std::int8_t as_small(double value, int otherwise = -1)
{
    return static_cast<std::int8_t>(std::isnan(value) ? otherwise : std::lround(value));
}

// Days since 1970-01-01 of a civil date, Howard Hinnant's way.
std::int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2 ? 1 : 0;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

// "2026-10-25T16:15:00", in UTC as Buienradar gives it, or with an offset
// after it, "+00:00", as MeteoAlarm does.
bool unix_of(const char *text, std::size_t length, std::int64_t &out)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    char copy[32];
    const std::size_t n = std::min(length, sizeof(copy) - 1);
    std::memcpy(copy, text, n);
    copy[n] = '\0';
    if (std::sscanf(copy, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) < 5) {
        return false;
    }
    out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    constexpr std::size_t ZONE_AT = 19;  // after "YYYY-MM-DDTHH:MM:SS"
    int                   zh = 0, zm = 0;
    if (n > ZONE_AT && (copy[ZONE_AT] == '+' || copy[ZONE_AT] == '-') &&
        std::sscanf(copy + ZONE_AT + 1, "%2d:%2d", &zh, &zm) == 2) {
        out -= (copy[ZONE_AT] == '+' ? 1 : -1) * (zh * 3600 + zm * 60);
    }
    return true;
}

// Inside a ring, by counting the edges a line east from the point crosses.
template <typename At>
bool inside_ring(float lat, float lon, int count, At at)
{
    bool in = false;
    for (int i = 0, j = count - 1; i < count; j = i++) {
        float lat_i = 0, lon_i = 0, lat_j = 0, lon_j = 0;
        at(i, lat_i, lon_i);
        at(j, lat_j, lon_j);
        if ((lat_i > lat) != (lat_j > lat) && lon < lon_i + (lat - lat_i) / (lat_j - lat_i) * (lon_j - lon_i)) {
            in = !in;
        }
    }
    return in;
}

// The text between <tag> and </tag> in [from, to), or false.
bool element(const char *from, const char *to, const char *tag, const char *&start, std::size_t &length)
{
    char open[40], close[40];
    std::snprintf(open, sizeof(open), "<%s>", tag);
    std::snprintf(close, sizeof(close), "</%s>", tag);
    const std::string_view within(from, static_cast<std::size_t>(to - from));
    const std::size_t      a = within.find(open);
    if (a == std::string_view::npos) {
        return false;
    }
    const std::size_t b = within.find(close, a);
    if (b == std::string_view::npos) {
        return false;
    }
    start  = from + a + std::strlen(open);
    length = b - a - std::strlen(open);
    return true;
}

// CAP's "lat,lon lat,lon ...", round home.
bool polygon_holds(const char *text, std::size_t length, float lat, float lon)
{
    constexpr int MAX_POINTS = 512;
    static float  lats[MAX_POINTS], lons[MAX_POINTS];
    int           count = 0;
    const char   *p = text, *end = text + length;
    while (p < end && count < MAX_POINTS) {
        char *stop = nullptr;
        const float a = std::strtof(p, &stop);
        if (stop == p || stop >= end || *stop != ',') {
            break;
        }
        p             = stop + 1;
        const float b = std::strtof(p, &stop);
        if (stop == p) {
            break;
        }
        lats[count] = a;
        lons[count] = b;
        ++count;
        p = stop;
        while (p < end && (*p == ' ' || *p == '\n')) {
            ++p;
        }
    }
    return count >= 3 && inside_ring(lat, lon, count, [](int i, float &la, float &lo) { la = lats[i], lo = lons[i]; });
}

// "Moderate wind warning", "Severe Wind", "Moderate high-temperature warning": "wind", "heat".
void what_of(const char *event, std::size_t length, char *out, std::size_t size)
{
    std::string words(event, length);
    for (char &c : words) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const char *drop : {"minor ", "moderate ", "severe ", "extreme ", " warning"}) {
        const std::size_t at = words.find(drop);
        if (at != std::string::npos) {
            words.erase(at, std::strlen(drop));
        }
    }
    const char *plain = words == "high-temperature" ? "heat"
                      : words == "low-temperature"  ? "cold"
                      : words == "snow-ice"         ? "snow and ice"
                      : words == "coastalevent"     ? "the coast"
                                                    : words.c_str();
    std::snprintf(out, size, "%s", plain);
}

bool read_current(Scanner &s, Now &now, bool &found)
{
    return s.members([&](const char *k, std::size_t n) {
        double v = 0;
        if (is(k, n, "time")) {
            if (!s.number(v)) {
                return false;
            }
            now.time = static_cast<std::int64_t>(v);
            found    = true;
            return true;
        }
        float *into = is(k, n, "temperature_2m")       ? &now.temp_c
                    : is(k, n, "apparent_temperature") ? &now.feels_c
                    : is(k, n, "wind_speed_10m")       ? &now.wind_kmh
                    : is(k, n, "wind_direction_10m")   ? &now.wind_from_deg
                                                       : nullptr;
        if (into != nullptr) {
            if (!s.number(v)) {
                return false;
            }
            *into = as_float(v);
            return true;
        }
        if (is(k, n, "weather_code")) {
            if (!s.number(v)) {
                return false;
            }
            now.code = as_small(v);
            return true;
        }
        if (is(k, n, "is_day")) {
            if (!s.number(v)) {
                return false;
            }
            now.day = v > 0.5;
            return true;
        }
        return false;
    });
}

bool read_hourly(Scanner &s, Forecast &out)
{
    return s.members([&](const char *k, std::size_t n) {
        Hour *hours = out.hours;
        if (is(k, n, "time")) {
            return s.numbers([&](int i, double v) {
                if (i < kHoursMax) {
                    hours[i].time  = static_cast<std::int64_t>(v);
                    out.hour_count = std::max(out.hour_count, i + 1);
                }
            });
        }
        if (is(k, n, "temperature_2m")) {
            return s.numbers([&](int i, double v) {
                if (i < kHoursMax) hours[i].temp_c = as_float(v);
            });
        }
        if (is(k, n, "weather_code")) {
            return s.numbers([&](int i, double v) {
                if (i < kHoursMax) hours[i].code = as_small(v);
            });
        }
        if (is(k, n, "is_day")) {
            return s.numbers([&](int i, double v) {
                if (i < kHoursMax) hours[i].day = !std::isnan(v) && v > 0.5;
            });
        }
        return false;
    });
}

bool read_minutely(Scanner &s, std::int64_t *times, float *mm, int capacity, int &count)
{
    return s.members([&](const char *k, std::size_t n) {
        if (is(k, n, "time")) {
            return s.numbers([&](int i, double v) {
                if (i < capacity) {
                    times[i] = static_cast<std::int64_t>(v);
                    count    = std::max(count, i + 1);
                }
            });
        }
        if (is(k, n, "precipitation")) {
            return s.numbers([&](int i, double v) {
                if (i < capacity) mm[i] = as_float(v);
            });
        }
        return false;
    });
}

bool read_daily(Scanner &s, Forecast &out)
{
    return s.members([&](const char *k, std::size_t n) {
        Day *days = out.days;
        const auto when = [&](std::int64_t Day::*field) {
            return s.numbers([&](int i, double v) {
                if (i < kDaysMax) {
                    days[i].*field = static_cast<std::int64_t>(v);
                    if (field == &Day::date) {
                        out.day_count = std::max(out.day_count, i + 1);
                    }
                }
            });
        };
        const auto amount = [&](float Day::*field) {
            return s.numbers([&](int i, double v) {
                if (i < kDaysMax) days[i].*field = as_float(v);
            });
        };
        if (is(k, n, "time")) return when(&Day::date);
        if (is(k, n, "sunrise")) return when(&Day::sunrise);
        if (is(k, n, "sunset")) return when(&Day::sunset);
        if (is(k, n, "temperature_2m_min")) return amount(&Day::low_c);
        if (is(k, n, "temperature_2m_max")) return amount(&Day::high_c);
        return false;
    });
}

}  // namespace

bool parse_forecast(const char *body, std::size_t length, Forecast &out)
{
    out          = {};
    bool    have = false;
    Scanner s{body, body + length};
    constexpr int QUARTERS = (kRainMax - 1) / 3;
    std::int64_t  quarter_at[QUARTERS] = {};
    float         quarter_mm[QUARTERS] = {};
    int           quarters             = 0;
    const bool ok = s.members([&](const char *k, std::size_t n) {
        if (is(k, n, "utc_offset_seconds")) {
            double v = 0;
            if (!s.number(v)) {
                return false;
            }
            out.utc_offset_s = static_cast<std::int32_t>(v);
            return true;
        }
        if (is(k, n, "current")) return read_current(s, out.now, have);
        if (is(k, n, "hourly")) return read_hourly(s, out);
        if (is(k, n, "daily")) return read_daily(s, out);
        if (is(k, n, "minutely_15")) return read_minutely(s, quarter_at, quarter_mm, QUARTERS, quarters);
        return false;
    });
    // each quarter as three fives, a quarter's millimetres as so many an hour
    constexpr int STEP_S = 5 * 60, QUARTERS_AN_HOUR = 4;
    for (int q = 0; q < quarters; ++q) {
        for (int k = 0; k < 3 && out.soon_count < kRainMax; ++k) {
            const std::int64_t at     = quarter_at[q] + k * STEP_S + out.utc_offset_s;
            const auto         minute = static_cast<std::int16_t>(((at / 60) % 1440 + 1440) % 1440);
            out.soon[out.soon_count++] = {minute, quarter_mm[q] * QUARTERS_AN_HOUR};
        }
    }
    return ok && have;
}

float rain_mm_h(int value)
{
    return value <= 0 ? 0.0f : std::pow(10.0f, (static_cast<float>(value) - 109.0f) / 32.0f);
}

int parse_rain(const char *body, std::size_t length, RainStep *out, int capacity)
{
    int         count = 0;
    const char *p     = body;
    const char *end   = body + length;
    while (p < end && count < capacity) {
        const char *line_end = static_cast<const char *>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
        if (line_end == nullptr) {
            line_end = end;
        }
        char line[24];
        const std::size_t n = std::min(static_cast<std::size_t>(line_end - p), sizeof(line) - 1);
        std::memcpy(line, p, n);
        line[n]   = '\0';
        int value = 0, h = 0, m = 0;
        if (std::sscanf(line, "%d|%d:%d", &value, &h, &m) == 3 && h >= 0 && h < 24 && m >= 0 && m < 60) {
            out[count++] = {static_cast<std::int16_t>(h * 60 + m), rain_mm_h(value)};
        }
        p = line_end + 1;
    }
    return count;
}

bool rain_covers(float home_lat, float home_lon)
{
    // Buienradar's frame, with half a degree to spare
    return home_lon > 0.48f && home_lon < 9.56f && home_lat > 49.86f && home_lat < 54.28f;
}

const char *province_of(float lat, float lon)
{
    for (const Province &province : kProvinces) {
        const auto at = [&province](int i, float &la, float &lo) {
            la = static_cast<float>(province.points[i * 2]) / 1e6f;
            lo = static_cast<float>(province.points[i * 2 + 1]) / 1e6f;
        };
        if (inside_ring(lat, lon, province.count, at)) {
            return province.emma;
        }
    }
    return nullptr;
}

int parse_warnings(const char *body, std::size_t length, float lat, float lon, std::int64_t now, Warning *out,
                   int capacity)
{
    const char      *province = province_of(lat, lon);
    int              count    = 0;
    const std::string_view all(body, length);
    std::size_t      at       = 0;
    while ((at = all.find("<entry>", at)) != std::string_view::npos) {
        const std::size_t close = all.find("</entry>", at);
        if (close == std::string_view::npos) {
            break;
        }
        const char *from = body + at, *to = body + close;
        at               = close;
        const char *text = nullptr;
        std::size_t len  = 0;
        // for home: its province's code, or an area KNMI drew round it
        bool here = province != nullptr && element(from, to, "value", text, len) &&
                    std::string_view(text, len) == province;
        if (!here && element(from, to, "cap:polygon", text, len)) {
            here = polygon_holds(text, len, lat, lon);
        }
        if (!here || !element(from, to, "cap:severity", text, len)) {
            continue;
        }
        const std::string_view severity(text, len);
        const Code code = severity == "Moderate" ? Code::Yellow
                        : severity == "Severe"   ? Code::Orange
                        : severity == "Extreme"  ? Code::Red
                                                 : Code::None;
        Warning w{};
        w.code = code;
        if (code == Code::None || !element(from, to, "cap:event", text, len)) {
            continue;
        }
        what_of(text, len, w.what, sizeof(w.what));
        if (element(from, to, "cap:onset", text, len)) {
            unix_of(text, len, w.onset);
        }
        if (!element(from, to, "cap:expires", text, len) || !unix_of(text, len, w.expires) || w.expires <= now) {
            continue;
        }
        // one for each kind, the more severe
        int same = -1;
        for (int i = 0; i < count && same < 0; ++i) {
            same = std::strcmp(out[i].what, w.what) == 0 ? i : -1;
        }
        if (same >= 0) {
            if (w.code > out[same].code) {
                out[same] = w;
            }
        } else if (count < capacity) {
            out[count++] = w;
        }
    }
    std::sort(out, out + count, [](const Warning &a, const Warning &b) { return a.code > b.code; });
    return count;
}

Sky sky_of(int code)
{
    switch (code) {
    case 0: return Sky::Clear;
    case 1:
    case 2: return Sky::Partly;
    case 3: return Sky::Cloudy;
    case 45:
    case 48: return Sky::Fog;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57: return Sky::Drizzle;
    case 61:
    case 63:
    case 65:
    case 66:
    case 67: return Sky::Rain;
    case 80:
    case 81:
    case 82: return Sky::Showers;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86: return Sky::Snow;
    case 95:
    case 96:
    case 99: return Sky::Thunder;
    default: return Sky::Unknown;
    }
}

const char *words_of(int code)
{
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: return "Fog";
    case 48: return "Freezing fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56:
    case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66:
    case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85:
    case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96:
    case 99: return "Thunderstorm with hail";
    default: return "";
    }
}

const char *compass_short_of(float from_deg)
{
    static const char *const NAMES[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    constexpr float POINT = 360.0f / 16.0f;
    const float     deg   = std::fmod(std::fmod(from_deg, 360.0f) + 360.0f, 360.0f);
    return NAMES[static_cast<int>(std::lround(deg / POINT)) % 16];
}

}  // namespace weather
