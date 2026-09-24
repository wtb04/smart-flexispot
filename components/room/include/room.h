#pragma once

#include "esp_err.h"
#include "ha_ws_protocol.h"

#include <string>
#include <vector>
#include "ui.h"

namespace room {
/** Scaffolds the home screen before any connect. */
void init();

/** Every entity the page shows or acts on, for the websocket to subscribe to:
 *  one list, rather than a second copy to keep in step by hand. */
std::vector<std::string> entities();

/** Runs on the WebSocket task. */
void render(const hass::ws::EntityStore &store);

/** How many entities the WebSocket store holds. */
int entity_count();

void on_media(ui::MediaAction action);

void on_setpoint(float celsius);

void on_mode();

/** The large lights button: runs the all-on or all-off script. */
void on_lights();

/** One light in the long-press picker. */
void on_light(int index);

/** One of the thermostat card's corner toggles. */
void on_dial_toggle(int index);

}  // namespace room
