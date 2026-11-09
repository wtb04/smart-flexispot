#include "weather.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jobs.h"
#include "net.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <utility>

namespace weather {
namespace {
constexpr char TAG[] = "weather";

constexpr char FORECAST_HOST[] = "https://api.open-meteo.com";
constexpr char RAIN_HOST[]     = "https://gadgets.buienradar.nl";
constexpr char WARNINGS_HOST[] = "https://feeds.meteoalarm.org";

constexpr std::size_t FORECAST_MAX = 24 * units::kBytesPerKiB;
constexpr std::size_t RAIN_MAX     = 4 * units::kBytesPerKiB;
constexpr std::size_t WARNINGS_MAX = 160 * units::kBytesPerKiB;  // the whole country's, some 50 KB on a quiet day

// Home's forecast every quarter hour and its rain to come every five minutes,
// as Buienradar has it new.
constexpr std::int64_t FORECAST_EVERY_US = 15 * units::kUsPerMinute;
constexpr std::int64_t RAIN_EVERY_US     = 5 * units::kUsPerMinute;
constexpr std::int64_t WARNINGS_EVERY_US = 15 * units::kUsPerMinute;
constexpr std::int64_t RETRY_US          = units::kUsPerMinute;
constexpr int          CHECK_MS          = 10 * units::kMsPerSecond;
constexpr float        HOME_GRID_DEG     = 0.01f;  // as the radar keeps it

SemaphoreHandle_t s_lock = nullptr;
StaticSemaphore_t s_lock_ctrl;

jobs::Job     s_job       = jobs::kNoJob;
UpdateHandler s_on_update = nullptr;
net::Host     s_forecast_host = net::kNoHost, s_rain_host = net::kNoHost, s_warnings_host = net::kNoHost;

char     *s_body = nullptr;  // WARNINGS_MAX, the largest answer
Forecast *s_forecast = nullptr, *s_forecast_next = nullptr;
bool      s_has_forecast = false;
RainStep  s_rain[kRainMax];
int       s_rain_count = 0;
Warning   s_warnings[kWarningsMax];
int       s_warning_count = 0;

std::atomic<float> s_lat{0.0f}, s_lon{0.0f};
std::atomic<bool>  s_has_home{false};

std::int64_t s_forecast_due = 0, s_rain_due = 0, s_warnings_due = 0;

struct Held {
    Held() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Held() { xSemaphoreGive(s_lock); }
};

void add_hosts()
{
    net::HostConfig forecast;
    forecast.name        = "open-meteo";
    forecast.base        = FORECAST_HOST;
    forecast.timeout_ms  = 20 * units::kMsPerSecond;
    forecast.connections = 1;
    forecast.gzip        = true;
    forecast.retry       = net::Retry{1, 2 * units::kMsPerSecond, 200, false};
    forecast.rest        = net::Rest{3, 5 * units::kMsPerMinute, 10 * units::kMsPerMinute};
    s_forecast_host      = net::add_host(forecast);

    net::HostConfig rain;
    rain.name        = "buienradar";
    rain.base        = RAIN_HOST;
    rain.connections = 1;
    rain.retry       = net::Retry{1, units::kMsPerSecond, 200, false};
    s_rain_host      = net::add_host(rain);

    net::HostConfig warnings;
    warnings.name        = "meteoalarm";
    warnings.base        = WARNINGS_HOST;
    warnings.timeout_ms  = 15 * units::kMsPerSecond;
    warnings.connections = 1;
    warnings.gzip        = true;
    warnings.retry       = net::Retry{1, 2 * units::kMsPerSecond, 200, false};
    s_warnings_host      = net::add_host(warnings);
}

void home_of(float &lat, float &lon)
{
    lat = s_lat.load(std::memory_order_relaxed);
    lon = s_lon.load(std::memory_order_relaxed);
}

net::Fetched get(net::Host host, const char *path, const char *what, std::size_t max)
{
    net::Request request;
    request.host     = host;
    request.path     = path;
    request.priority = net::Priority::Background;
    request.key      = what;
    request.dedupe   = net::Dedupe::Replace;
    request.max_body = max - 1;
    request.what     = what;  // never where home is
    return net::fetch(std::move(request), s_body, max);
}

bool fetch_forecast()
{
    float lat = 0, lon = 0;
    home_of(lat, lon);
    char path[512];
    std::snprintf(path, sizeof(path),
                  "/v1/forecast?latitude=%.2f&longitude=%.2f"
                  "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m,is_day"
                  "&hourly=temperature_2m,weather_code,is_day"
                  "&daily=temperature_2m_max,temperature_2m_min,sunrise,sunset"
                  "&minutely_15=precipitation&forecast_minutely_15=8"
                  "&forecast_days=%d&forecast_hours=%d&timezone=auto&timeformat=unixtime",
                  static_cast<double>(lat), static_cast<double>(lon), kDaysMax, kHoursMax);
    const net::Fetched got = get(s_forecast_host, path, "forecast", FORECAST_MAX);
    if (!got.ok() || !parse_forecast(s_body, got.length, *s_forecast_next)) {
        ESP_LOGW(TAG, "forecast: %d", got.status);
        return false;
    }
    const Held held;
    std::swap(s_forecast, s_forecast_next);
    s_has_forecast = true;
    return true;
}

bool fetch_rain()
{
    float lat = 0, lon = 0;
    home_of(lat, lon);
    char path[96];
    std::snprintf(path, sizeof(path), "/data/raintext?lat=%.2f&lon=%.2f", static_cast<double>(lat),
                  static_cast<double>(lon));
    const net::Fetched got = get(s_rain_host, path, "rain", RAIN_MAX);
    RainStep           steps[kRainMax];
    const int          n = got.ok() ? parse_rain(s_body, got.length, steps, kRainMax) : 0;
    if (n == 0) {
        ESP_LOGW(TAG, "rain: %d", got.status);
        return false;
    }
    const Held held;
    std::copy(steps, steps + n, s_rain);
    s_rain_count = n;
    return true;
}

// KNMI's warnings, from MeteoAlarm's feed for the Netherlands, those for home kept.
bool fetch_warnings()
{
    float lat = 0, lon = 0;
    home_of(lat, lon);
    const net::Fetched got = get(s_warnings_host, "/feeds/meteoalarm-legacy-atom-netherlands", "warnings", WARNINGS_MAX);
    if (!got.ok()) {
        ESP_LOGW(TAG, "warnings: %d", got.status);
        return false;
    }
    Warning   found[kWarningsMax];
    const int n = parse_warnings(s_body, got.length, lat, lon, std::time(nullptr), found, kWarningsMax);
    const Held held;
    std::copy(found, found + n, s_warnings);
    s_warning_count = n;
    return true;
}

jobs::Result check()
{
    if (!s_has_home.load(std::memory_order_relaxed)) {
        return jobs::sleep();  // poked when home comes
    }
    const std::int64_t now = esp_timer_get_time();
    const auto due = [now](std::int64_t &at, std::int64_t every, bool (*fetch)()) {
        if (now < at) {
            return;
        }
        const bool got = fetch();
        at             = now + (got ? every : RETRY_US);
        if (got && s_on_update != nullptr) {
            s_on_update();
        }
    };
    float lat = 0, lon = 0;
    home_of(lat, lon);
    due(s_forecast_due, FORECAST_EVERY_US, fetch_forecast);
    if (rain_covers(lat, lon)) {
        due(s_rain_due, RAIN_EVERY_US, fetch_rain);
    }
    if (province_of(lat, lon) != nullptr) {
        due(s_warnings_due, WARNINGS_EVERY_US, fetch_warnings);
    }
    return jobs::done();
}
}  // namespace

esp_err_t start(UpdateHandler on_update)
{
    ESP_RETURN_ON_FALSE(s_job == jobs::kNoJob, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_ctrl);
    const auto psram = [](std::size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); };
    s_body          = static_cast<char *>(psram(WARNINGS_MAX));
    s_forecast      = static_cast<Forecast *>(psram(sizeof(Forecast)));
    s_forecast_next = static_cast<Forecast *>(psram(sizeof(Forecast)));
    ESP_RETURN_ON_FALSE(s_lock != nullptr && s_body != nullptr && s_forecast != nullptr && s_forecast_next != nullptr,
                        ESP_ERR_NO_MEM, TAG, "buffers");
    s_on_update = on_update;
    add_hosts();
    jobs::Spec spec;
    spec.name      = TAG;
    spec.lane      = jobs::Lane::Slow;
    spec.period_ms = CHECK_MS;
    spec.first_ms  = jobs::kNever;
    spec.online    = true;
    spec.lit       = true;  // nothing to see in the dark
    spec.run       = check;
    s_job          = jobs::add(std::move(spec));
    ESP_RETURN_ON_FALSE(s_job != jobs::kNoJob, ESP_ERR_NO_MEM, TAG, "job");
    return ESP_OK;
}

void set_home(float lat, float lon)
{
    lat = std::round(lat / HOME_GRID_DEG) * HOME_GRID_DEG;
    lon = std::round(lon / HOME_GRID_DEG) * HOME_GRID_DEG;
    const bool lat_moved = s_lat.exchange(lat) != lat;
    const bool lon_moved = s_lon.exchange(lon) != lon;  // both stored, whichever moved
    if (!lat_moved && !lon_moved && s_has_home.load()) {
        return;
    }
    // somewhere else: everything again, at once
    s_forecast_due = s_rain_due = s_warnings_due = 0;
    s_has_home = true;
    jobs::poke(s_job);
}

bool forecast(Forecast &out)
{
    if (s_lock == nullptr) {
        return false;
    }
    const Held held;
    out = *s_forecast;
    return s_has_forecast;
}

int rain(RainStep *out, int capacity)
{
    if (s_lock == nullptr) {
        return 0;
    }
    const Held held;
    const bool      radar = s_rain_count > 0;
    const RainStep *from  = radar ? s_rain : s_forecast->soon;
    const int       n     = std::min(capacity, radar ? s_rain_count : s_has_forecast ? s_forecast->soon_count : 0);
    std::copy(from, from + n, out);
    return n;
}

int warnings(Warning *out, int capacity)
{
    if (s_lock == nullptr) {
        return 0;
    }
    const Held held;
    const int  n = std::min(capacity, s_warning_count);
    std::copy(s_warnings, s_warnings + n, out);
    return n;
}

}  // namespace weather
