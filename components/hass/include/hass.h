#pragma once

#include "esp_err.h"
#include "hass_protocol.h"

namespace hass {

/** Called on the MQTT task when Home Assistant asks for something. */
struct Handlers {
    void (*on_preset)(int preset);                            // 1-4
    void (*on_brightness)(int percent);                       // 0-100
    void (*on_notify)(const protocol::Notification &notice);  // show on screen
    void (*on_move)(protocol::Move direction);                // up / down / stop
};

/** Connects to the broker and starts publishing. Safe to call before Wi-Fi is up. */
esp_err_t start(const Handlers &handlers);

/** Publishes telemetry if it has changed since last time. Cheap to call often. */
esp_err_t publish(const protocol::Telemetry &telemetry);

/** True while the broker connection is established. */
bool connected();

}  // namespace hass
