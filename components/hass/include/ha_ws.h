#pragma once

#include "esp_err.h"
#include "ha_ws_protocol.h"

#include <string>
#include <vector>

namespace hass::ws {
/** Invoked from the socket task whenever the entity store changes. */
using UpdateHandler = void (*)(const EntityStore &store);

/** Safe to call before Wi-Fi is up: net connects it when there is a network,
 *  and again whenever it drops. Subscribes to `entities` and nothing else. */
esp_err_t start(UpdateHandler on_update, std::vector<std::string> entities,
                std::vector<std::string> attributes = {});

/** True once authenticated and subscribed. */
bool connected();

/** Where Home Assistant serves plain HTTP, such as pictures: the websocket's
 *  scheme and host without the path. Empty when none is configured. */
const char *http_origin();

/** Called when a service call did not happen: refused by Home Assistant, or
 *  not sent because the socket was down. From the socket or the caller's task. */
using RefusalHandler = void (*)(const char *reason);

void on_refusal(RefusalHandler handler);

/** The last service call that did not happen, and how many seconds ago. False
 *  if none has been refused since boot. Thread-safe. */
bool last_refusal(char *out, std::size_t size, int &age_s);

esp_err_t call_service(const char *domain, const char *service, const char *entity_id);

/** What a reply carries under `result`, or null when it failed, or the link went
 *  before it came. On the socket task, or the caller's when it was never sent. */
using ReplyHandler = void (*)(const cJSON *result);

/** Sends `body`, a command as a JSON object without its id, and hands the reply
 *  to `on_reply`. */
esp_err_t request(const std::string &body, ReplyHandler on_reply);

esp_err_t call_service_with(const char *domain, const char *service, const char *entity_id,
                            const char *field, const char *value);

}  // namespace hass::ws
