#pragma once

#include "esp_err.h"
#include "ha_ws_protocol.h"

#include <string>
#include <vector>

namespace hass::ws {
/** Invoked from the socket task whenever the entity store changes. */
using UpdateHandler = void (*)(const EntityStore &store);

/** Safe to call before Wi-Fi is up; the client retries on its own. Subscribes
 *  to `entities` and nothing else. */
esp_err_t start(UpdateHandler on_update, std::vector<std::string> entities);

/** True once authenticated and subscribed. */
bool connected();

/** Where Home Assistant serves plain HTTP, such as pictures: the websocket's
 *  scheme and host without the path. Empty when none is configured. */
const char *http_origin();

/** Stops and starts the client, for when its own retries have got nowhere.
 *  Creates it if it never was. */
esp_err_t restart();

/** Called when a service call did not happen: refused by Home Assistant, or
 *  not sent because the socket was down. From the socket or the caller's task. */
using RefusalHandler = void (*)(const char *reason);

void on_refusal(RefusalHandler handler);

esp_err_t call_service(const char *domain, const char *service, const char *entity_id);

esp_err_t call_service_with(const char *domain, const char *service, const char *entity_id,
                            const char *field, const char *value);

}  // namespace hass::ws
