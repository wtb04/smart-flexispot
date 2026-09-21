#pragma once

#include "esp_err.h"
#include "ha_ws_protocol.h"

namespace hass::ws {

/** Invoked from the socket task whenever the entity store changes. */
using UpdateHandler = void (*)(const EntityStore &store);

/**
 * @brief Connects to Home Assistant and subscribes to the configured entities.
 *
 * Safe to call before Wi-Fi is up; the client retries on its own.
 */
esp_err_t start(UpdateHandler on_update);

/** True once authenticated and subscribed. */
bool connected();

/** Asks Home Assistant to run a service, e.g. light.turn_on. */
esp_err_t call_service(const char *domain, const char *service, const char *entity_id);

/** As above, carrying one field such as a setpoint or an hvac mode. */
esp_err_t call_service_with(const char *domain, const char *service, const char *entity_id,
                            const char *field, const char *value);

}  // namespace hass::ws
