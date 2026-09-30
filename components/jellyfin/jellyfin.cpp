#include "jellyfin.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "net_stream.h"
#include "units.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#if __has_include("jellyfin_secrets.h")
#include "jellyfin_secrets.h"
#endif
#ifndef JELLYFIN_URL
#define JELLYFIN_URL ""
#define JELLYFIN_KEY ""
#endif

namespace jellyfin {
namespace {
constexpr char TAG[] = "jellyfin";

constexpr char DEVICE_ID[] = "smart-flexispot";  // so its own session is known and skipped

// The server pushes whenever a player reports, a pause at once and progress
// every ten seconds, besides each interval. With the queue's items a session
// runs to more than half a megabyte, which pushed every second the panel could
// not take in fast enough, falling ever further behind; so the interval is
// left long and the reports carry it.
constexpr int SESSIONS_INTERVAL_MS = 60 * units::kMsPerSecond;
// The server drops a socket it has not heard from in its keep-alive time, 60 s
// as it says; half that is what its own clients send at.
constexpr int KEEP_ALIVE_MS = 30 * units::kMsPerSecond;

constexpr int         BUFFER_SIZE        = 16 * units::kBytesPerKiB;  // fewer reads for the big ones
constexpr int         SOCKET_STACK       = 6144;
constexpr int         NETWORK_TIMEOUT_MS = 10 * units::kMsPerSecond;
constexpr std::size_t MAX_MESSAGE        = 64 * units::kBytesPerKiB;  // once trimmed
constexpr int         HTTP_TIMEOUT_MS    = 5 * units::kMsPerSecond;

constexpr int           COMMAND_DEADLINE_MS = 10 * units::kMsPerSecond;
constexpr std::size_t   COMMAND_ANSWER_MAX  = 4 * units::kBytesPerKiB;
constexpr std::size_t   LOOKUP_ANSWER_MAX   = 16 * units::kBytesPerKiB;

constexpr char HTTPS[] = "https://";
constexpr char WSS[]   = "wss://";


net::Stream  s_stream    = net::kNoStream;
Handler      s_on_change = nullptr;
std::string  s_rx;
Trimmer      s_trim;
bool         s_rx_dropped  = false;  // the message coming in is over the most
std::int64_t s_rx_began_us = 0;      // when the message's first bytes came

std::mutex s_now_lock;
NowPlaying s_now;  // what was last handed on

// The player reports subtitles with its progress, every ten seconds, so what
// was asked for stands until then: a second tap toggles back rather than asks
// again, and the button does not fall back meanwhile.
constexpr std::int64_t SUBTITLE_HOLD_US = 15 * units::kUsPerSecond;
int                    s_subtitle_asked    = -1;

std::int64_t           s_subtitle_asked_at = 0;  // 0 when nothing waits

bool same(const NowPlaying &a, const NowPlaying &b)
{
    return a.active == b.active && a.paused == b.paused && a.session == b.session &&
           a.item == b.item && a.position_s == b.position_s && a.duration_s == b.duration_s &&
           a.title == b.title && a.volume == b.volume && a.subtitle == b.subtitle &&
           a.subtitle_track == b.subtitle_track;
}

void take_sessions(const std::string &message)
{
    NowPlaying now = now_playing(message, DEVICE_ID);
    // Its position was true when it began to arrive; a session with its queue
    // takes seconds to come in whole, and playing on goes on meanwhile.
    if (now.active && !now.paused && s_rx_began_us != 0) {
        now.position_s += static_cast<int>((esp_timer_get_time() - s_rx_began_us) / units::kUsPerSecond);
    }
    {
        std::lock_guard<std::mutex> hold(s_now_lock);
        if (s_subtitle_asked_at != 0) {
            if (now.subtitle == s_subtitle_asked || now.item != s_now.item ||
                esp_timer_get_time() - s_subtitle_asked_at > SUBTITLE_HOLD_US) {
                s_subtitle_asked_at = 0;
            } else {
                now.subtitle = s_subtitle_asked;
            }
        }
        if (same(now, s_now)) {
            return;
        }
        s_now = now;
    }
    if (s_on_change != nullptr) {
        s_on_change(now);
    }
}

// Subtitles off, or on with the default track, from what was last asked for
// if the player has not said yet; handed on at once as though it had.
int toggle_subtitle_locally()
{
    NowPlaying now;
    {
        std::lock_guard<std::mutex> hold(s_now_lock);
        if (!s_now.active || s_now.subtitle_track < 0) {
            return -2;
        }
        s_now.subtitle      = s_now.subtitle >= 0 ? -1 : s_now.subtitle_track;
        s_subtitle_asked    = s_now.subtitle;
        s_subtitle_asked_at = esp_timer_get_time();
        now                 = s_now;
    }
    if (s_on_change != nullptr) {
        s_on_change(now);
    }
    return now.subtitle;
}

// Sessions come in a piece at a time and are trimmed as they come, as a
// session with its queue runs to more than half a megabyte.
void take_piece(const net::Fragment &piece)
{
    if (piece.first) {
        s_rx.clear();
        s_trim.reset();
        s_rx_dropped  = false;
        s_rx_began_us = esp_timer_get_time();
    }
    if (!s_rx_dropped) {
        s_trim.feed(piece.data, piece.length, s_rx);
        if (s_rx.size() > MAX_MESSAGE) {
            ESP_LOGW(TAG, "message over %u bytes, dropping", static_cast<unsigned>(MAX_MESSAGE));
            s_rx.clear();
            s_rx_dropped = true;
        }
    }
    if (piece.last) {
        if (!s_rx_dropped && message_type(s_rx) == "Sessions") {
            take_sessions(s_rx);
        }
        s_rx.clear();
    }
}

void on_open()
{
    s_rx.clear();
    if (net::stream_send(s_stream, sessions_start(SESSIONS_INTERVAL_MS))) {
        ESP_LOGI(TAG, "connected, following sessions");
        net::stream_ready(s_stream);
    } else {
        net::stream_fail(s_stream, "could not ask for sessions");
    }
}

void on_close()
{
    s_rx.clear();
    s_rx_began_us = 0;
    take_sessions("");  // nothing known to be playing while away
}

// Commands and lookups go through net, on one connection kept for the
// server, so they reach the player in the order they were given.
net::Host s_host = net::kNoHost;

void add_host()
{
    static const std::string headers = std::string("Authorization: MediaBrowser Token=\"") + JELLYFIN_KEY + "\"";
    net::HostConfig config;
    config.name        = "jellyfin";
    config.base        = JELLYFIN_URL;
    config.headers     = headers.c_str();
    config.timeout_ms  = HTTP_TIMEOUT_MS;
    config.connections = 1;
    config.idle_ms     = units::kMsPerMinute;
    config.retry       = net::Retry{1, 300, 200, false};
    config.rest        = net::Rest{3, 10 * units::kMsPerSecond, 30 * units::kMsPerSecond};
    s_host             = net::add_host(config);
}

// The session to send to, if it takes what is asked of it; a player that only
// reports would ignore it, and its next report would undo what the card showed.
std::string playing_session(bool NowPlaying::*takes = &NowPlaying::remote)
{
    std::lock_guard<std::mutex> hold(s_now_lock);
    return s_now.active && s_now.*takes ? s_now.session : "";
}

// Posted to the player's session. With a key, a newer one of its kind takes
// the place of one still waiting: a slider dragged along sends where it
// stopped, not every level it passed.
void command(const std::string &path, const std::string &body, const char *what, const char *key = nullptr)
{
    net::Request request;
    request.host        = s_host;
    request.path        = path;
    request.method      = net::Method::Post;
    request.body        = body;
    request.priority    = net::Priority::Tap;
    request.deadline_ms = COMMAND_DEADLINE_MS;
    request.max_body    = COMMAND_ANSWER_MAX;
    request.what        = what;
    if (key != nullptr) {
        request.key    = key;
        request.dedupe = net::Dedupe::Replace;
    }
    request.done = [what](const net::Response &answer) {
        if (answer.ok()) {
            ESP_LOGI(TAG, "sent %s", what);
        } else if (answer.outcome != net::Outcome::Replaced) {
            ESP_LOGW(TAG, "%s not taken: %s, http %d", what, net::outcome_name(answer.outcome), answer.status);
        }
    };
    net::submit(std::move(request));
}

std::string socket_uri()
{
    std::string base = JELLYFIN_URL;
    if (base.rfind(HTTPS, 0) == 0) {
        base = WSS + base.substr(std::strlen(HTTPS));
    }
    return base + "/socket?api_key=" + JELLYFIN_KEY + "&deviceId=" + DEVICE_ID;
}
}  // namespace

esp_err_t start(Handler on_change)
{
    if (JELLYFIN_URL[0] == '\0' || JELLYFIN_KEY[0] == '\0') {
        ESP_LOGW(TAG, "no Jellyfin address or key, not starting");
        return ESP_OK;
    }
    s_on_change = on_change;
    add_host();

    net::WebsocketConfig config;
    config.name                 = TAG;
    config.uri                  = socket_uri();
    config.task_stack           = SOCKET_STACK;
    config.buffer_size          = BUFFER_SIZE;
    config.network_timeout_ms   = NETWORK_TIMEOUT_MS;
    config.tls_bundle           = true;
    config.assemble             = false;
    config.policy.keep_alive_ms = KEEP_ALIVE_MS;

    net::StreamHandlers on;
    on.opened     = on_open;
    on.closed     = on_close;
    on.fragment   = take_piece;
    on.keep_alive = [] { return keep_alive(); };
    return net::open_websocket(s_stream, config, std::move(on));
}

bool connected()
{
    return net::stream_is_ready(s_stream);
}

bool fetch(const std::string &path, std::string &out)
{
    net::Request request;
    request.host     = s_host;
    request.path     = path;
    request.priority = net::Priority::Now;
    request.max_body = LOOKUP_ANSWER_MAX;
    request.what     = "lookup";
    return net::fetch(std::move(request), out).ok() && !out.empty();
}

// The button turns at once, and a second tap before the player reports
// carries on rather than asks to pause again; the player's report corrects it
// should the server not take it.
void play_pause()
{
    NowPlaying now;
    bool       pause = false;
    {
        std::lock_guard<std::mutex> hold(s_now_lock);
        if (!s_now.active || !s_now.remote) {
            return;
        }
        pause        = !s_now.paused;
        s_now.paused = pause;
        now          = s_now;
    }
    if (s_on_change != nullptr) {
        s_on_change(now);
    }
    command(pause_path(now.session, pause), "", pause ? "pause" : "carry on");
}

void seek(int position_s)
{
    const std::string session = playing_session();
    if (!session.empty()) {
        command(seek_path(session, position_s), "", "seek", "seek");
    }
}

void set_volume(int percent)
{
    const std::string session = playing_session(&NowPlaying::takes_volume);
    if (!session.empty()) {
        command(command_path(session), set_volume_body(percent), "volume", "volume");
    }
}

void toggle_subtitles()
{
    const std::string session = playing_session(&NowPlaying::takes_subtitles);
    if (session.empty()) {
        return;
    }
    const int stream = toggle_subtitle_locally();
    if (stream >= -1) {
        command(command_path(session), set_subtitle_body(stream), "subtitles", "subtitles");
    }
}

void play_now(const std::string &item)
{
    const std::string session = playing_session();
    if (!item.empty() && !session.empty()) {
        command(play_now_path(session, item), "", "play", "play");
    }
}

std::string cover_url(const std::string &item, int height)
{
    return JELLYFIN_URL + cover_path(item, height);
}
}  // namespace jellyfin
