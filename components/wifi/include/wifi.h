#pragma once

#include "esp_err.h"

namespace wifi {
/** Returns as soon as the attempt is under way. An empty SSID starts nothing.
 *  From then on a task keeps the link up: reconnects with backoff, restarts
 *  the association after three minutes without an address, the radio after
 *  ten, and keeps trying if the radio would not come up at all. */
esp_err_t start();

/** True once an IP address has been assigned. */
bool connected();

bool wait_for_ip(int timeout_ms);

}  // namespace wifi
