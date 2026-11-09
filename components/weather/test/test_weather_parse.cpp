#include "weather_parse.h"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
using namespace weather;

std::string read_file(const char *name)
{
    const std::string path = std::string(FIXTURES) + "/" + name;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    std::string text;
    char        chunk[4096];
    std::size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        text.append(chunk, got);
    }
    std::fclose(file);
    return text;
}

TEST(Forecast, ReadsNowHoursAndDays)
{
    const std::string json = read_file("open_meteo_point.json");
    Forecast          f{};
    ASSERT_TRUE(parse_forecast(json.c_str(), json.size(), f));
    EXPECT_EQ(f.utc_offset_s, 3600);
    EXPECT_EQ(f.now.time, 1792944900);
    EXPECT_FLOAT_EQ(f.now.temp_c, 12.6f);
    EXPECT_FLOAT_EQ(f.now.feels_c, 4.9f);
    EXPECT_FLOAT_EQ(f.now.wind_kmh, 43.2f);
    EXPECT_FLOAT_EQ(f.now.wind_from_deg, 271.0f);
    EXPECT_EQ(f.now.code, 2);
    EXPECT_TRUE(f.now.day);

    // as many as are kept of the 48 recorded
    ASSERT_EQ(f.hour_count, kHoursMax);
    EXPECT_EQ(f.hours[0].time, 1792944000);
    EXPECT_FLOAT_EQ(f.hours[0].temp_c, 12.6f);
    EXPECT_EQ(f.hours[0].code, 3);
    EXPECT_EQ(f.hours[kHoursMax - 1].time, 1792944000 + (kHoursMax - 1) * 3600);

    ASSERT_EQ(f.day_count, kDaysMax);
    EXPECT_EQ(f.days[0].date, 1792882800);
    EXPECT_FLOAT_EQ(f.days[0].low_c, 10.9f);
    EXPECT_FLOAT_EQ(f.days[0].high_c, 13.1f);
    EXPECT_EQ(f.days[0].sunrise, 1792909433);
    EXPECT_EQ(f.days[0].sunset, 1792945521);
}

TEST(Forecast, WithoutCurrentIsNothing)
{
    const char json[] = R"({"latitude":52.3,"hourly":{"time":[1,2],"temperature_2m":[1.0,2.0]}})";
    Forecast   f{};
    EXPECT_FALSE(parse_forecast(json, sizeof(json) - 1, f));
}

TEST(Forecast, NullsAreUnknown)
{
    const char json[] = R"({"current":{"time":5,"temperature_2m":null},"hourly":{"time":[1,2],)"
                        R"("weather_code":[null,3],"is_day":[null,1]}})";
    Forecast   f{};
    ASSERT_TRUE(parse_forecast(json, sizeof(json) - 1, f));
    EXPECT_FLOAT_EQ(f.now.temp_c, 0.0f);
    EXPECT_EQ(f.hours[0].code, -1);
    EXPECT_FALSE(f.hours[0].day);
    EXPECT_EQ(f.hours[1].code, 3);
    EXPECT_TRUE(f.hours[1].day);
}

TEST(Forecast, QuartersOfRainAsFives)
{
    // 17:00 and 17:15 local, an hour east of UTC
    const char json[] = R"({"utc_offset_seconds":3600,"current":{"time":1},)"
                        R"("minutely_15":{"time":[1792944000,1792944900],"precipitation":[0.0,0.5]}})";
    Forecast   f{};
    ASSERT_TRUE(parse_forecast(json, sizeof(json) - 1, f));
    ASSERT_EQ(f.soon_count, 6);
    EXPECT_EQ(f.soon[0].minute, 17 * 60);
    EXPECT_EQ(f.soon[2].minute, 17 * 60 + 10);
    EXPECT_EQ(f.soon[3].minute, 17 * 60 + 15);
    EXPECT_FLOAT_EQ(f.soon[0].mm_h, 0.0f);
    EXPECT_FLOAT_EQ(f.soon[5].mm_h, 2.0f);
}

TEST(Rain, ReadsFiveMinuteSteps)
{
    const std::string text = read_file("buienradar_rain.txt");
    RainStep          steps[kRainMax];
    const int         n = parse_rain(text.c_str(), text.size(), steps, kRainMax);
    ASSERT_EQ(n, 24);
    EXPECT_EQ(steps[0].minute, 17 * 60 + 20);
    EXPECT_FLOAT_EQ(steps[0].mm_h, 0.0f);
}

TEST(Rain, ValuesAreMillimetresAnHour)
{
    const char text[] = "077|17:10\r\n087|17:15\r\n099|17:20\r\n109|17:25\r\ngarbage\n000|17:30";
    RainStep   steps[8];
    ASSERT_EQ(parse_rain(text, sizeof(text) - 1, steps, 8), 5);
    EXPECT_NEAR(steps[0].mm_h, 0.1f, 0.001f);
    EXPECT_NEAR(steps[1].mm_h, 0.205f, 0.001f);
    EXPECT_NEAR(steps[2].mm_h, 0.487f, 0.001f);
    EXPECT_FLOAT_EQ(steps[3].mm_h, 1.0f);
    EXPECT_EQ(steps[4].minute, 17 * 60 + 30);
    EXPECT_FLOAT_EQ(steps[4].mm_h, 0.0f);
}

TEST(Rain, CoversOnlyRoundTheNetherlands)
{
    EXPECT_TRUE(rain_covers(52.31f, 4.76f));
    EXPECT_TRUE(rain_covers(50.85f, 4.35f));
    EXPECT_FALSE(rain_covers(51.5f, -0.1f));
    EXPECT_FALSE(rain_covers(48.9f, 2.35f));
}

TEST(Warnings, ProvinceOfHome)
{
    EXPECT_STREQ(province_of(51.98f, 5.91f), "NL014");  // Arnhem, Gelderland
    EXPECT_STREQ(province_of(52.08f, 4.30f), "NL009");  // Den Haag, Zuid-Holland
    EXPECT_STREQ(province_of(53.22f, 6.57f), "NL007");  // Groningen
    EXPECT_EQ(province_of(51.5f, -0.1f), nullptr);      // London
    EXPECT_EQ(province_of(50.85f, 4.35f), nullptr);     // Brussels
}

TEST(Warnings, ForHomeOnly)
{
    const std::string   xml = read_file("meteoalarm_netherlands.xml");
    Warning             w[kWarningsMax];
    const std::int64_t  now = 1792946000;  // 2026-10-25 17:33 UTC
    // Den Haag: Zuid-Holland's code yellow for wind
    int n = parse_warnings(xml.c_str(), xml.size(), 52.08f, 4.30f, now, w, kWarningsMax);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(w[0].code, Code::Yellow);
    EXPECT_STREQ(w[0].what, "wind");
    EXPECT_EQ(w[0].expires, 1792962000);  // 2026-10-25 21:00 UTC
    // Texel: Noord-Holland's yellow and the island's own orange, the orange kept
    n = parse_warnings(xml.c_str(), xml.size(), 53.06f, 4.80f, now, w, kWarningsMax);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(w[0].code, Code::Orange);
    // Arnhem: none for Gelderland
    EXPECT_EQ(parse_warnings(xml.c_str(), xml.size(), 51.98f, 5.91f, now, w, kWarningsMax), 0);
    // once over, none
    EXPECT_EQ(parse_warnings(xml.c_str(), xml.size(), 52.08f, 4.30f, 1793200000, w, kWarningsMax), 0);
}

TEST(Words, SkyAndWind)
{
    EXPECT_EQ(sky_of(0), Sky::Clear);
    EXPECT_EQ(sky_of(3), Sky::Cloudy);
    EXPECT_EQ(sky_of(63), Sky::Rain);
    EXPECT_EQ(sky_of(81), Sky::Showers);
    EXPECT_EQ(sky_of(95), Sky::Thunder);
    EXPECT_EQ(sky_of(42), Sky::Unknown);
    EXPECT_STREQ(words_of(3), "Overcast");
    EXPECT_STREQ(words_of(42), "");
    EXPECT_STREQ(compass_short_of(202.0f), "SSW");
    EXPECT_STREQ(compass_short_of(271.0f), "W");
    EXPECT_STREQ(compass_short_of(-10.0f), "N");
    EXPECT_STREQ(compass_short_of(355.0f), "N");
}

}  // namespace
