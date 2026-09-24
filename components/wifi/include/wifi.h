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

/** What the Wi-Fi task last read off the radio, refreshed every few seconds.
 *  Reading it costs nothing; asking the radio directly is a round trip to the
 *  C6. Empty strings for anything not known. */
struct Info {
    bool connected = false;
    bool have_ap   = false;  // ssid, rssi and channel are from the access point
    char ssid[33]  = {};
    char ip[16]    = {};
    char mac[18]   = {};
    int  rssi_dbm  = 0;
    int  channel   = 0;
};

Info info();

}  // namespace wifi
