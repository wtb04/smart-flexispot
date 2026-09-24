#pragma once

// Copy to hass_secrets.h (git-ignored) and fill in. These end up in the
// firmware image, so a flash dump reveals them. An empty URI never connects.

#define HASS_MQTT_URI      "mqtt://192.168.1.10:1883"
#define HASS_MQTT_USER     ""
#define HASS_MQTT_PASSWORD ""

// Prefixes every topic. Changing it later leaves the old entities as ghosts.
#define HASS_DEVICE_ID "tab5_desk"

// WebSocket API, for reading other entities and calling services; an empty URL
// skips it. Token: your profile -> Security -> Long-lived access tokens.
#define HASS_WS_URI   "ws://homeassistant.local:8123/api/websocket"
#define HASS_WS_TOKEN ""

