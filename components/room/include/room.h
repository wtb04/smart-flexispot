#pragma once

#include "esp_err.h"
#include "ha_ws_protocol.h"
#include "jellyfin_protocol.h"
#include "laptop_protocol.h"

#include <string>
#include <vector>
#include "ui.h"

namespace room {
/** Scaffolds the home screen before any connect. */
void init();

/** Every entity the page shows or acts on, for the websocket to subscribe to:
 *  one list, rather than a second copy to keep in step by hand. */
std::vector<std::string> entities();

/** Every attribute the page reads, so the rest are not kept. */
std::vector<std::string> attributes();

/** Runs on the WebSocket task. */
void render(const hass::ws::EntityStore &store);

/** How many entities the WebSocket store holds. */
int entity_count();

void on_media(ui::MediaAction action);

/** What Jellyfin's followed session plays, from its socket's task. */
void on_jellyfin(const jellyfin::NowPlaying &now);

/** What Desk Link says plays on a laptop, from the server's task. */
void on_laptop(const laptop::NowPlaying &now);

/** Jumps the playing video to `position_s`. */
void on_seek(int position_s);
void on_media_volume(int percent);  // as the cinema's slider sets it

/** Plays one of the favourites the media card offers. */
void on_pick(int index);

void on_setpoint(float celsius);

void on_mode();

/** The large lights button: runs the all-on or all-off script. */
void on_lights();

/** One light in the long-press picker. */
void on_light(int index);

/** One of the thermostat card's corner toggles. */
void on_dial_toggle(int index);

}  // namespace room
