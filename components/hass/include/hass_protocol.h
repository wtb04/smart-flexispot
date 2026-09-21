#pragma once

#include <cstdint>
#include <string>

namespace hass::protocol {

enum class Move { Stop, Up, Down, Unknown };

struct Telemetry {
    int  height_mm      = -1;     // negative: not yet known
    bool desk_connected = false;
    const char *motion  = "idle";  // idle | moving_up | moving_down
    int  battery_percent = -1;    // negative: no pack fitted
    float battery_volts  = 0.0f;
    int   battery_milliamps = 0;
    bool  charging          = false;
    bool  on_battery        = false;
    int   brightness    = 0;
    int   rssi_dbm      = 0;
    bool  presence          = false;
    int   presence_rssi     = -127;
    std::uint32_t uptime_s   = 0;
    std::uint32_t free_heap  = 0;
    std::string   ip_address;
};

struct Topics {
    std::string availability;  // also the last will
    std::string state;
    std::string discovery;
    std::string command;       // wildcard subscription
    std::string cmd_preset;
    std::string cmd_brightness;
    std::string cmd_notify;
    std::string cmd_move;
};

Topics topics_for(const std::string &device_id);

std::string state_document(const Telemetry &telemetry);

/**
 * One retained message for all entities; Home Assistant 2024.11 and later.
 * Emits no `object_id`: deprecated in 2025.10, removed in 2026.4.
 */
std::string discovery_document(const std::string &device_id, const std::string &sw_version);

/** Shown on screen. Attacker-controlled text; treat as data. */
struct Notification {
    bool        valid = false;
    std::string title;
    std::string message;
    std::string level;  // info | success | warning | error
    int         timeout_ms = 0;
};

/** Accepts plain text or a JSON object; rejects broken JSON and control bytes. */
Notification parse_notification(const std::string &payload);

Move parse_move(const std::string &payload);

/** 0 when the payload names no preset. */
int parse_preset(const std::string &payload);

/** -1 when the payload is not a number in 0-100. */
int parse_brightness(const std::string &payload);

}  // namespace hass::protocol
