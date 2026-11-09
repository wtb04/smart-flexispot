// The weather as weather.cpp has it on the panel, home's forecast, its rain to
// come and KNMI's warnings, read by the same parsers from the answers recorded
// in SIM_WEATHER_FILES. Nothing is asked for: the panel shares Open-Meteo's
// daily limit from the same address. Without the folder the weather stays empty.
#include "live.h"
#include "weather.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace weather {
namespace {
std::mutex            s_lock;
Forecast              s_forecast{};
bool                  s_has_forecast = false;
std::vector<RainStep> s_rain;
std::vector<Warning>  s_warnings;

std::string recorded(const char *dir, const char *file)
{
    std::string text;
    if (std::FILE *f = std::fopen((std::string(dir) + "/" + file).c_str(), "rb")) {
        char        chunk[4096];
        std::size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
            text.append(chunk, got);
        }
        std::fclose(f);
    }
    return text;
}
}  // namespace

esp_err_t start(UpdateHandler)
{
    return ESP_OK;
}

void set_home(float, float) {}

bool forecast(Forecast &out)
{
    std::lock_guard<std::mutex> hold(s_lock);
    out = s_forecast;
    return s_has_forecast;
}

int rain(RainStep *out, int capacity)
{
    std::lock_guard<std::mutex> hold(s_lock);
    if (!s_rain.empty()) {
        const int n = std::min<int>(capacity, static_cast<int>(s_rain.size()));
        std::copy(s_rain.begin(), s_rain.begin() + n, out);
        return n;
    }
    const int n = s_has_forecast ? std::min(capacity, s_forecast.soon_count) : 0;
    std::copy(s_forecast.soon, s_forecast.soon + n, out);
    return n;
}

int warnings(Warning *out, int capacity)
{
    std::lock_guard<std::mutex> hold(s_lock);
    const int n = std::min<int>(capacity, static_cast<int>(s_warnings.size()));
    std::copy(s_warnings.begin(), s_warnings.begin() + n, out);
    return n;
}
}  // namespace weather

namespace live {
using namespace weather;

bool read_weather()
{
    const char *dir = std::getenv("SIM_WEATHER_FILES");
    if (dir == nullptr) {
        return false;
    }
    const std::string forecast = recorded(dir, "forecast.json");
    const std::string rain     = recorded(dir, "rain.txt");
    const std::string warnings = recorded(dir, "warnings.xml");
    std::lock_guard<std::mutex> hold(s_lock);
    s_has_forecast = parse_forecast(forecast.c_str(), forecast.size(), s_forecast);
    RainStep steps[kRainMax];
    s_rain.assign(steps, steps + parse_rain(rain.c_str(), rain.size(), steps, kRainMax));
    // KNMI's for SIM_HOME, as the panel's for Home Assistant's home
    float lat = 0, lon = 0;
    if (const char *home = std::getenv("SIM_HOME"); home != nullptr && std::sscanf(home, "%f,%f", &lat, &lon) == 2) {
        Warning found[kWarningsMax];
        s_warnings.assign(found, found + parse_warnings(warnings.c_str(), warnings.size(), lat, lon,
                                                        std::time(nullptr), found, kWarningsMax));
    }
    std::fprintf(stderr, "weather: %s from %s\n", s_has_forecast ? "the forecast" : "no forecast", dir);
    return true;
}
}  // namespace live
