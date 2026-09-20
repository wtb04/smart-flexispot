#pragma once

#include "esp_err.h"

namespace wifi {

/**
 * @brief Joins the network named in wifi_secrets.h and keeps trying.
 *
 * Returns as soon as the attempt is under way; joining happens in the
 * background. An image built with an empty SSID starts nothing.
 */
esp_err_t start();

/** True once an IP address has been assigned. */
bool connected();

}  // namespace wifi
