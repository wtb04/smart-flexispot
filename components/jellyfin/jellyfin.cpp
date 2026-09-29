#include "jellyfin.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
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
constexpr TickType_t KEEP_ALIVE = pdMS_TO_TICKS(30 * units::kMsPerSecond);

constexpr int         BUFFER_SIZE        = 16 * units::kBytesPerKiB;  // fewer reads for the big ones
constexpr int         SOCKET_STACK       = 6144;
constexpr int         NETWORK_TIMEOUT_MS = 10 * units::kMsPerSecond;
constexpr int         RECONNECT_MS       = 5 * units::kMsPerSecond;
constexpr TickType_t  SEND_TIMEOUT       = pdMS_TO_TICKS(5 * units::kMsPerSecond);
constexpr std::size_t MAX_MESSAGE        = 64 * units::kBytesPerKiB;  // once trimmed
constexpr int         HTTP_TIMEOUT_MS    = 5 * units::kMsPerSecond;

constexpr int WS_OPCODE_CONTINUATION = 0x00;
constexpr int WS_OPCODE_TEXT         = 0x01;

constexpr std::uint32_t KEEP_ALIVE_STACK    = 3072;
constexpr UBaseType_t   KEEP_ALIVE_PRIORITY = 3;
constexpr BaseType_t    KEEP_ALIVE_CORE     = 0;
constexpr int           COMMAND_DEADLINE_MS = 10 * units::kMsPerSecond;
constexpr std::size_t   COMMAND_ANSWER_MAX  = 4 * units::kBytesPerKiB;
constexpr std::size_t   LOOKUP_ANSWER_MAX   = 16 * units::kBytesPerKiB;

constexpr char HTTPS[] = "https://";
constexpr char WSS[]   = "wss://";


esp_websocket_client_handle_t s_client   = nullptr;
Handler                       s_on_change = nullptr;
std::string                   s_rx;
Trimmer                       s_trim;
std::int64_t                  s_rx_began_us = 0;  // when the message's first bytes came

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

bool send_text(const std::string &text)
{
    return s_client != nullptr && esp_websocket_client_is_connected(s_client) &&
           esp_websocket_client_send_text(s_client, text.data(), static_cast<int>(text.size()),
                                          SEND_TIMEOUT) >= 0;
}

void handle_data(const esp_websocket_event_data_t *event)
{
    if (event->op_code == WS_OPCODE_TEXT) {
        if (event->payload_offset == 0) {
            s_rx.clear();
            s_trim.reset();
            s_rx_began_us = esp_timer_get_time();
        }
    } else if (event->op_code != WS_OPCODE_CONTINUATION || s_rx.empty()) {
        return;  // binary, ping, pong, close: not ours
    }
    s_trim.feed(event->data_ptr, static_cast<std::size_t>(event->data_len), s_rx);
    if (s_rx.size() > MAX_MESSAGE) {
        ESP_LOGW(TAG, "message over %u bytes, dropping", static_cast<unsigned>(MAX_MESSAGE));
        s_rx.clear();
        return;
    }
    if (event->payload_offset + event->data_len < event->payload_len || !event->fin) {
        return;
    }
    if (message_type(s_rx) == "Sessions") {
        take_sessions(s_rx);
    }
    s_rx.clear();
}

void on_event(void *, esp_event_base_t, std::int32_t id, void *data)
{
    switch (static_cast<esp_websocket_event_id_t>(id)) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected, following sessions");
            s_rx.clear();
            send_text(sessions_start(SESSIONS_INTERVAL_MS));
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            s_rx.clear();
            s_rx_began_us = 0;
            take_sessions("");  // nothing known to be playing while away
            break;
        case WEBSOCKET_EVENT_DATA:
            handle_data(static_cast<esp_websocket_event_data_t *>(data));
            break;
        default:
            break;
    }
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

std::string playing_session()
{
    std::lock_guard<std::mutex> hold(s_now_lock);
    return s_now.active ? s_now.session : "";
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

// Keeps the socket alive between the server's pushes.
[[noreturn]] void keep_alive_task(void *)
{
    for (;;) {
        vTaskDelay(KEEP_ALIVE);
        send_text(keep_alive());
    }
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

    static StaticTask_t keep_alive_ctrl;
    auto *stack = static_cast<StackType_t *>(heap_caps_malloc(
        KEEP_ALIVE_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(stack != nullptr &&
                            xTaskCreateStaticPinnedToCore(keep_alive_task, "jellyfin", KEEP_ALIVE_STACK,
                                                          nullptr, KEEP_ALIVE_PRIORITY, stack,
                                                          &keep_alive_ctrl, KEEP_ALIVE_CORE) != nullptr,
                        ESP_ERR_NO_MEM, TAG, "keep-alive task");

    static std::string uri = socket_uri();
    esp_websocket_client_config_t cfg{};
    cfg.uri                    = uri.c_str();
    cfg.task_stack             = SOCKET_STACK;
    cfg.buffer_size            = BUFFER_SIZE;
    cfg.network_timeout_ms     = NETWORK_TIMEOUT_MS;
    cfg.reconnect_timeout_ms   = RECONNECT_MS;
    cfg.enable_close_reconnect = true;
    cfg.crt_bundle_attach      = esp_crt_bundle_attach;
    s_client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != nullptr, ESP_FAIL, TAG, "init");
    ESP_RETURN_ON_ERROR(
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_event, nullptr), TAG,
        "events");
    return esp_websocket_client_start(s_client);
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
        if (!s_now.active) {
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
    const std::string session = playing_session();
    if (!session.empty()) {
        command(command_path(session), set_volume_body(percent), "volume", "volume");
    }
}

void toggle_subtitles()
{
    const int stream = toggle_subtitle_locally();
    const std::string session = playing_session();
    if (stream >= -1 && !session.empty()) {
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
