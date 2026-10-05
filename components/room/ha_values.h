#pragma once

#include "ha_ws_protocol.h"

#include <string>

// Reading Home Assistant's entities, for the room and the speaker alike.
namespace room {
inline constexpr float kNoNumber        = -1.0f;  // attribute_number()'s answer when there is none
inline constexpr float kPercentPerWhole = 100.0f;

bool        is_on(const std::string &state);
bool        known(const hass::ws::Entity *entity);
std::string attribute(const hass::ws::Entity &entity, const char *key);
float       attribute_number(const hass::ws::Entity &entity, const char *key);
std::string upper(std::string text);
int         percent_of(float fraction);

/** Seconds since a Home Assistant timestamp, or 0 if it cannot be read. */
int seconds_since(const std::string &iso);

/** A series, and its season and episode on a line under it. */
std::string episode_line(const std::string &series, int season, int episode);
}  // namespace room
