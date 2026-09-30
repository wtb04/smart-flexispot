#pragma once

#include "esp_err.h"
#include "hass_protocol.h"

namespace hass {
/** Called on the MQTT task when Home Assistant asks for something. */
struct Handlers {
    void (*on_preset)(int preset);                            // 1 to kPresetCount
    void (*on_brightness)(int percent);                       // 0-100
    void (*on_notify)(const protocol::Notification &notice);
    void (*on_move)(protocol::Move direction);
    void (*on_screen)(bool on);
};

/** Safe to call before Wi-Fi is up: net connects it when there is a network.
 *  `brightness_floor` is the dimmest the
 *  panel goes, for Home Assistant's slider. */
esp_err_t start(const Handlers &handlers, int brightness_floor);

/** Publishes only what has changed, so it is cheap to call often. */
esp_err_t publish(const protocol::Telemetry &telemetry);

bool connected();

}  // namespace hass
