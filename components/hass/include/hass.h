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

/** Safe to call before Wi-Fi is up. `brightness_floor` is the dimmest the
 *  panel goes, for Home Assistant's slider. */
esp_err_t start(const Handlers &handlers, int brightness_floor);

/** Publishes only what has changed, so it is cheap to call often. */
esp_err_t publish(const protocol::Telemetry &telemetry);

bool connected();

/** For when the client's own retries have got nowhere: alternately nudges it
 *  to retry now, and stops and starts it. Creates it if it never was. */
esp_err_t restart();

}  // namespace hass
