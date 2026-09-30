#pragma once

#include "esp_err.h"

namespace wifi {
/** The longest name 802.11 allows a network. */
inline constexpr int kSsidMax = 32;

/** Returns as soon as the attempt is under way. An empty SSID starts nothing.
 *  From then on a task keeps the link up: reconnects with backoff, restarts
 *  the association after three minutes without an address, the radio after
 *  ten, and keeps trying if the radio would not come up at all. */
esp_err_t start();

/** What to do when the co-processor stays out of reach, as it has after a
 *  plain restart: board::restart_cold(). Before start(). */
void on_radio_stuck(void (*restart_cold)());

/** True once an IP address has been assigned. */
bool connected();

bool wait_for_ip(int timeout_ms);

/** What the Wi-Fi task last read off the radio, refreshed every few seconds.
 *  Reading it costs nothing; asking the radio directly is a round trip to the
 *  C6. Empty strings for anything not known. */
struct Info {
    bool connected                        = false;
    bool have_ap                          = false;  // ssid, rssi and channel are from the AP
    char ssid[kSsidMax + 1]               = {};
    char ip[sizeof("255.255.255.255")]    = {};
    char mac[sizeof("00:00:00:00:00:00")] = {};
    int  rssi_dbm                         = 0;
    int  channel                          = 0;
};

Info info();

}  // namespace wifi
