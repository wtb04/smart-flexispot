#pragma once

#include "esp_err.h"
#include "ha_ws_protocol.h"

namespace room {

/** Scaffolds the home screen with the entities it will show, before any connect. */
void init();

/** Pushes the entities Home Assistant reports onto the home screen. */
void render(const hass::ws::EntityStore &store);

/** UI tile handler: acts on the entity behind that tile, if it has one. */
void on_tile(int index);

/** Thermostat dial released: asks Home Assistant for that setpoint. */
void on_setpoint(float celsius);

/** Thermostat mode button: turns the heating on or off. */
void on_mode();

/** The large lights button: runs the all-on or all-off script. */
void on_lights();

/** One light in the long-press picker. */
void on_light(int index);

/** One of the thermostat card's corner toggles. */
void on_dial_toggle(int index);

}  // namespace room
