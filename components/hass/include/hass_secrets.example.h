#pragma once

// Copy to hass_secrets.h (git-ignored) and fill in your broker. Compiled into
// the firmware, so anyone with a flash dump can read them.
//
// An empty URI builds an image that never connects.

#define HASS_MQTT_URI      "mqtt://192.168.1.10:1883"
#define HASS_MQTT_USER     ""
#define HASS_MQTT_PASSWORD ""

// Prefixes every topic and identifies the device in Home Assistant. Changing
// it after the fact leaves the old entities behind as ghosts.
#define HASS_DEVICE_ID "tab5_desk"
