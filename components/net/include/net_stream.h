#pragma once

#include "esp_err.h"
#include "mqtt_client.h"
#include "stream_core.h"

#include <cstddef>
#include <functional>
#include <string>

// Live connections, kept as net keeps requests: connected once there is a
// network and at once when it comes back, begun again after a back-off when
// they drop or their other end refuses, stopped when they open and never get
// going, kept alive, and reported on in one place. What they carry stays with
// whoever opened them.
//
//     net::WebsocketConfig config;
//     config.name = "ha_ws";
//     config.uri  = "ws://homeassistant.local:8123/api/websocket";
//     net::StreamHandlers on;
//     on.opened  = [] { /* the socket is open: sign in */ };
//     on.message = [](const std::string &text) { /* ... net::stream_ready(s) once signed in */ };
//     net::open_websocket(s_stream, config, std::move(on));
namespace net {

using Stream                      = int;
inline constexpr Stream kNoStream = -1;

/** A piece of a text message as it comes in. */
struct Fragment {
    const char *data   = nullptr;
    std::size_t length = 0;
    bool        first  = false;
    bool        last   = false;
};

/** All called on the transport's own task, but keep_alive, which is net's. */
struct StreamHandlers {
    std::function<void()>                        opened;
    std::function<void()>                        closed;      // however it went, perhaps twice
    std::function<void(const std::string &)>     message;     // a whole text message, when put together
    std::function<void(const Fragment &)>        fragment;    // or each piece of one, when not
    std::function<std::string()>                 keep_alive;  // sent while ready, every keep_alive_ms
    std::function<void(esp_mqtt_event_handle_t)> mqtt;        // MQTT: each event, after net has seen it
};

struct WebsocketConfig {
    const char *name               = "";  // in the log
    std::string uri;
    int         buffer_size        = 4 * 1024;
    int         task_stack         = 6 * 1024;
    int         network_timeout_ms = 10 * 1000;
    int         ping_interval_s    = 0;  // 0 for the client's own
    int         pingpong_timeout_s = 0;  // 0 for none
    bool        tls_bundle         = false;
    bool        assemble           = true;  // whole messages to `message`; false for pieces to `fragment`
    std::size_t max_message        = 64 * 1024;
    StreamPolicy policy{};
};

struct MqttConfig {
    const char              *name = "";
    esp_mqtt_client_config_t client{};  // its own reconnecting is turned off: net does that
    StreamPolicy             policy{};
};

/** Once for each connection; it connects when there is a network. `into` is
 *  set before it can, so the handlers may use it from their first call. */
esp_err_t open_websocket(Stream &into, const WebsocketConfig &config, StreamHandlers handlers);
esp_err_t open_mqtt(Stream &into, const MqttConfig &config, StreamHandlers handlers);

/** Sent on an open websocket; false when it is not open or the send failed. */
bool stream_send(Stream stream, const std::string &text);
/** For publishing and subscribing; the same client across reconnects. */
esp_mqtt_client_handle_t stream_mqtt(Stream stream);

/** Its session is going: signed in, subscribed. Until then it counts as not
 *  connected, and is begun again if it takes longer than ready_within_ms. */
void stream_ready(Stream stream);
/** Its other end refused: stopped, and begun again after `retry_ms`, or the
 *  next back-off when 0. From any task. */
void stream_fail(Stream stream, const char *why, int retry_ms = 0);
void stream_restart(Stream stream);

bool         stream_is_ready(Stream stream);
int          stream_count();
Stream       find_stream(const char *name);
const char  *stream_name(Stream stream);
StreamStatus stream_status(Stream stream);

}  // namespace net
