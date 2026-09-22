#pragma once

#include "esp_err.h"

namespace wifi {
/** Returns as soon as the attempt is under way. An empty SSID starts nothing. */
esp_err_t start();

/** True once an IP address has been assigned. */
bool connected();

bool wait_for_ip(int timeout_ms);

}  // namespace wifi
