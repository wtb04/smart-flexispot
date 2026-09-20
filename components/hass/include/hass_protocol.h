#pragma once

#include <cstdint>
#include <string>

// Everything here is transport-free and testable on a host: topic layout,
// discovery payload, the state document, and parsing of what comes back.
namespace hass::protocol {

/** Everything the panel knows about itself. Only real measurements belong here. */
/** What the desk is doing, as published and as commanded. */
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
    std::uint32_t uptime_s   = 0;
    std::uint32_t free_heap  = 0;
    std::string   ip_address;
};

struct Topics {
    std::string availability;  // retained "online"/"offline", also the LWT
    std::string state;         // retained JSON document
    std::string discovery;     // retained device-based discovery payload
    std::string command;       // wildcard subscription
    std::string cmd_preset;
    std::string cmd_brightness;
    std::string cmd_notify;
    std::string cmd_move;
};

Topics topics_for(const std::string &device_id);

/** The single retained JSON document every entity reads through a template. */
std::string state_document(const Telemetry &telemetry);

/**
 * @brief Device-based discovery payload: one retained message, all entities.
 *
 * Home Assistant 2024.11 and later. Deliberately does not emit `object_id`,
 * which was deprecated in 2025.10 and removed in 2026.4 -- a stale one is
 * ignored silently and entity ids drift.
 */
std::string discovery_document(const std::string &device_id, const std::string &sw_version);

/** Shown on screen. Everything here is attacker-controlled text; treat as data. */
struct Notification {
    bool        valid = false;
    std::string title;
    std::string message;
    std::string level;  // info | success | warning | error
    int         timeout_ms = 0;
};

/**
 * @brief Parses an inbound notification payload.
 *
 * Accepts plain text or a JSON object. Rejects anything that looks like JSON
 * but does not parse, and anything carrying control bytes, rather than
 * rendering it verbatim. Truncates without splitting a UTF-8 sequence.
 */
Notification parse_notification(const std::string &payload);

/** Returns the requested direction, or Move::Unknown for anything else. */
Move parse_move(const std::string &payload);

/** Returns 1-4 for a preset command, or 0 if the payload names no preset. */
int parse_preset(const std::string &payload);

/** Returns the requested brightness, or -1 if the payload is not a number in range. */
int parse_brightness(const std::string &payload);

}  // namespace hass::protocol
