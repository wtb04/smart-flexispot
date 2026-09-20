#pragma once

// Copy to wifi_secrets.h (git-ignored) and fill in the network to join. The
// credentials are compiled into the firmware, so changing network means
// editing this file and reflashing -- and anyone with the image can read them.
//
// An empty SSID builds an image that never joins anything.

#define TAB5_WIFI_SSID "your-network"
#define TAB5_WIFI_PASS "your-password"
