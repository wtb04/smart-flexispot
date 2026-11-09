#pragma once

#include <cstddef>
#include <cstdint>

namespace weather {
inline constexpr int kHoursMax = 24;
inline constexpr int kDaysMax  = 2;   // today's sun and tomorrow's
inline constexpr int kRainMax  = 25;  // Buienradar's two hours, in fives

/** The forecast at home, from Open-Meteo. Times are unix seconds. */
struct Now {
    float        temp_c;
    float        feels_c;
    float        wind_kmh;
    float        wind_from_deg;  // where it blows from, as weather reports give it
    std::int8_t  code;           // WMO weather code
    bool         day;
    std::int64_t time;
};

struct Hour {
    std::int64_t time;
    float        temp_c;
    std::int8_t  code;
    bool         day;
};

struct Day {
    std::int64_t date;  // local midnight
    std::int64_t sunrise;
    std::int64_t sunset;
    float        low_c;
    float        high_c;
};

/** Rain to come, a step every five minutes. `minute` is past local midnight. */
struct RainStep {
    std::int16_t minute;
    float        mm_h;
};

struct Forecast {
    Now          now;
    Hour         hours[kHoursMax];
    int          hour_count;
    Day          days[kDaysMax];
    int          day_count;
    std::int32_t utc_offset_s;
    // Open-Meteo's next two hours of rain, its quarters as three fives each,
    // for where Buienradar does not reach.
    RainStep     soon[kRainMax];
    int          soon_count;
};

/** Reads Open-Meteo's answer for one place. False when there is no current
 *  reading in it. */
bool parse_forecast(const char *body, std::size_t length, Forecast &out);

/** Buienradar's next two hours: "NNN|HH:MM" lines, as the answer gives them. */
int parse_rain(const char *body, std::size_t length, RainStep *out, int capacity);

/** Buienradar's 0 to 255 as millimetres an hour: 10^((value - 109) / 32). */
float rain_mm_h(int value);

/** Whether Buienradar's radar reaches home, as it does only round the
 *  Netherlands and Belgium. */
bool rain_covers(float home_lat, float home_lon);

/** KNMI's warnings as MeteoAlarm carries them: code yellow, orange or red,
 *  for what, and from when to when, unix seconds. */
inline constexpr int kWarningsMax    = 4;
inline constexpr int kWarningWhatMax = 24;

enum class Code : std::int8_t { None, Yellow, Orange, Red };

struct Warning {
    Code         code;
    char         what[kWarningWhatMax];  // "wind", "thunderstorm", "heat"
    std::int64_t onset;
    std::int64_t expires;
};

/** MeteoAlarm's code for the province home is in, "NL014"; null outside the
 *  Netherlands. */
const char *province_of(float lat, float lon);

/** The warnings in MeteoAlarm's Atom feed for the Netherlands that cover home,
 *  by its province or by one of KNMI's own areas drawn round it, still in force
 *  at `now`: one for each kind, the most severe, most severe first. */
int parse_warnings(const char *body, std::size_t length, float lat, float lon, std::int64_t now, Warning *out,
                   int capacity);

/** What the sky is doing, for a glyph and a word. */
enum class Sky : std::uint8_t { Clear, Partly, Cloudy, Fog, Drizzle, Rain, Showers, Snow, Thunder, Unknown };

Sky         sky_of(int code);
const char *words_of(int code);

/** The 16-point compass the wind blows from, as letters: "W", "NNE". */
const char *compass_short_of(float from_deg);

}  // namespace weather
