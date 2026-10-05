#include "ha_values.h"

#include "clock_math.h"
#include "units.h"

#include <cctype>
#include <cstdlib>
#include <ctime>

namespace room {
namespace {
constexpr std::size_t ISO_SECONDS_LENGTH = sizeof("YYYY-MM-DDTHH:MM:SS") - 1;
constexpr int         MAX_POSITION_AGE_S = units::kSecondsPerDay;
}  // namespace

bool is_on(const std::string &state)
{
    return state == "on" || state == "playing" || state == "heat" || state == "cool";
}

bool known(const hass::ws::Entity *entity)
{
    return entity != nullptr && entity->state != "unavailable" && entity->state != "unknown";
}

std::string attribute(const hass::ws::Entity &entity, const char *key)
{
    const auto it = entity.attributes.find(key);
    return it == entity.attributes.end() ? std::string{} : it->second;
}

float attribute_number(const hass::ws::Entity &entity, const char *key)
{
    const auto it = entity.attributes.find(key);
    if (it == entity.attributes.end()) {
        return kNoNumber;
    }
    char       *end   = nullptr;
    const float value = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? kNoNumber : value;
}

std::string upper(std::string text)
{
    for (char &c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

int percent_of(float fraction)
{
    return static_cast<int>(fraction * kPercentPerWhole + 0.5f);
}

int seconds_since(const std::string &iso)
{
    std::tm parsed{};
    if (iso.size() < ISO_SECONDS_LENGTH || strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &parsed) == nullptr) {
        return 0;
    }
    const std::time_t when = rtc::utc_seconds(parsed);
    const std::time_t now  = std::time(nullptr);
    const double      age  = std::difftime(now, when);
    return age > 0.0 && age < MAX_POSITION_AGE_S ? static_cast<int>(age) : 0;
}

std::string episode_line(const std::string &series, int season, int episode)
{
    std::string line = series;
    if (!line.empty() && season > 0 && episode > 0) {
        line += "\nSeason " + std::to_string(season) + ", episode " + std::to_string(episode);
    }
    return line;
}
}  // namespace room
