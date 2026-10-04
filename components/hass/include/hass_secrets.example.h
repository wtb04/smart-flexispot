#pragma once

// Copy to hass_secrets.h (git-ignored) and fill in. These end up in the
// firmware image, so a flash dump reveals them. An empty URI never connects.

// The MQTT broker the panel announces itself on, as "mqtt://192.168.1.10:1883".
// Give it a user of its own on the broker rather than leaving it open.
#define HASS_MQTT_URI      ""
#define HASS_MQTT_USER     ""
#define HASS_MQTT_PASSWORD ""

// Prefixes every topic. Changing it later leaves the old entities as ghosts.
#define HASS_DEVICE_ID "tab5_desk"

// WebSocket API, for reading other entities and calling services, as
// "ws://homeassistant.local:8123/api/websocket"; empty skips it. Token: your
// profile -> Security -> Long-lived access tokens, best from a user of its own.
#define HASS_WS_URI   ""
#define HASS_WS_TOKEN ""
