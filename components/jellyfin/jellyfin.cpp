#include "jellyfin.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "units.h"

#include <atomic>
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
constexpr int         HTTP_OK            = 200;

constexpr int WS_OPCODE_CONTINUATION = 0x00;
constexpr int WS_OPCODE_TEXT         = 0x01;

constexpr std::uint32_t WORKER_STACK    = 6144;  // TLS
constexpr UBaseType_t   WORKER_PRIORITY = 3;
constexpr BaseType_t    WORKER_CORE     = 0;
constexpr UBaseType_t   COMMAND_QUEUE   = 4;

constexpr char HTTPS[] = "https://";
constexpr char WSS[]   = "wss://";

constexpr std::size_t ITEM_SIZE = 40;  // an id, 32 hex digits

enum class Op : std::uint8_t { PlayPause, Seek, SetVolume, PlayNow, Subtitles };
struct Command {
    Op   op;
    int  value;            // the position jumped to, or the volume set
    char item[ITEM_SIZE];  // what is started now
};

esp_websocket_client_handle_t s_client   = nullptr;
Handler                       s_on_change = nullptr;
QueueHandle_t                 s_commands  = nullptr;
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

// Each command waits on the one before, a round trip through the server to
// the player, so a slider dragged along queued up a trail of levels: only the
// latest is sent, once the one before has gone.
std::atomic<int> s_volume_wanted{-1};
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

esp_http_client_handle_t open_client(const std::string &url, esp_http_client_method_t method)
{
    esp_http_client_config_t cfg{};
    cfg.url               = url.c_str();
    cfg.method            = method;
    cfg.timeout_ms        = HTTP_TIMEOUT_MS;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client != nullptr) {
        char authorization[96];
        std::snprintf(authorization, sizeof(authorization), "MediaBrowser Token=\"%s\"",
                      JELLYFIN_KEY);
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    return client;
}

// One connection kept for the commands, on the worker's task: a new one would
// cost a TLS handshake each, which a run of volume presses waits through.
esp_http_client_handle_t s_commander = nullptr;

void post(const std::string &path, const std::string &body = "")
{
    const std::string url = JELLYFIN_URL + path;
    if (s_commander == nullptr) {
        s_commander = open_client(url, HTTP_METHOD_POST);
        if (s_commander == nullptr) {
            return;
        }
    } else {
        esp_http_client_set_url(s_commander, url.c_str());
        esp_http_client_set_method(s_commander, HTTP_METHOD_POST);
    }
    // No body is null, not empty: the client gives an empty one a form's type,
    // and takes the type away again only for null.
    if (body.empty()) {
        esp_http_client_set_post_field(s_commander, nullptr, 0);
    } else {
        esp_http_client_set_header(s_commander, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_commander, body.data(), static_cast<int>(body.size()));
    }
    const esp_err_t err    = esp_http_client_perform(s_commander);
    const int       status = esp_http_client_get_status_code(s_commander);
    if (err != ESP_OK || status >= 300) {
        ESP_LOGW(TAG, "command refused (%s, %d)", esp_err_to_name(err), status);
    } else {
        ESP_LOGI(TAG, "sent %s", path.c_str());
    }
    if (err != ESP_OK) {
        esp_http_client_cleanup(s_commander);  // opened afresh for the next
        s_commander = nullptr;
    }
}

// Sends commands as they come, and keeps the socket alive between them.
[[noreturn]] void worker_task(void *)
{
    for (;;) {
        Command command{};
        if (xQueueReceive(s_commands, &command, KEEP_ALIVE) != pdTRUE) {
            send_text(keep_alive());
            continue;
        }
        std::string session;
        {
            std::lock_guard<std::mutex> hold(s_now_lock);
            session = s_now.active ? s_now.session : "";
        }
        if (session.empty()) {
            continue;
        }
        switch (command.op) {
            case Op::PlayPause: post(play_pause_path(session)); break;
            case Op::Seek:      post(seek_path(session, command.value)); break;
            case Op::SetVolume: {
                const int percent = s_volume_wanted.exchange(-1);
                if (percent >= 0) {
                    post(command_path(session), set_volume_body(percent));
                }
                break;
            }
            case Op::PlayNow:   post(play_now_path(session, command.item)); break;
            case Op::Subtitles: post(command_path(session), set_subtitle_body(command.value)); break;
        }
    }
}

void queue(Op op, int value = 0, const std::string &item = "")
{
    Command command{op, value, {}};
    std::snprintf(command.item, sizeof(command.item), "%s", item.c_str());
    if (s_commands != nullptr && xQueueSend(s_commands, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "too many commands at once");
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
    s_commands  = xQueueCreate(COMMAND_QUEUE, sizeof(Command));
    ESP_RETURN_ON_FALSE(s_commands != nullptr, ESP_ERR_NO_MEM, TAG, "command queue");

    static StaticTask_t worker_ctrl;
    auto *stack = static_cast<StackType_t *>(heap_caps_malloc(
        WORKER_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(stack != nullptr &&
                            xTaskCreateStaticPinnedToCore(worker_task, "jellyfin", WORKER_STACK,
                                                          nullptr, WORKER_PRIORITY, stack,
                                                          &worker_ctrl, WORKER_CORE) != nullptr,
                        ESP_ERR_NO_MEM, TAG, "worker");

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
    constexpr std::size_t MAX_ANSWER = 16 * units::kBytesPerKiB;
    esp_http_client_handle_t client = open_client(JELLYFIN_URL + path, HTTP_METHOD_GET);
    if (client == nullptr) {
        return false;
    }
    out.clear();
    bool ok = esp_http_client_open(client, 0) == ESP_OK &&
              esp_http_client_fetch_headers(client) >= 0 &&
              esp_http_client_get_status_code(client) == HTTP_OK;
    char chunk[512];
    while (ok && out.size() < MAX_ANSWER) {
        const int got = esp_http_client_read(client, chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }
        out.append(chunk, static_cast<std::size_t>(got));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok && !out.empty();
}

void play_pause()
{
    queue(Op::PlayPause);
}

void seek(int position_s)
{
    queue(Op::Seek, position_s);
}

void set_volume(int percent)
{
    if (s_volume_wanted.exchange(percent) < 0) {
        queue(Op::SetVolume);
    }
}

void toggle_subtitles()
{
    const int stream = toggle_subtitle_locally();
    if (stream >= -1) {
        queue(Op::Subtitles, stream);
    }
}

void play_now(const std::string &item)
{
    if (!item.empty()) {
        queue(Op::PlayNow, 0, item);
    }
}

std::string cover_url(const std::string &item, int height)
{
    return JELLYFIN_URL + cover_path(item, height);
}
}  // namespace jellyfin
