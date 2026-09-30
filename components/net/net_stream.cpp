#include "net_stream.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_state.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include <utility>

namespace net {
namespace {
constexpr int           MAX_STREAMS   = 6;
constexpr std::uint32_t TASK_STACK    = 4096;
constexpr UBaseType_t   TASK_PRIORITY = 3;
constexpr BaseType_t    TASK_CORE     = 0;
constexpr std::int64_t  LOOK_EVERY_US = 1000 * 1000;  // besides being woken
constexpr std::int64_t  SOONEST_US    = 10 * 1000;
constexpr TickType_t    SEND_TIMEOUT  = pdMS_TO_TICKS(5 * 1000);

constexpr int WS_OPCODE_CONTINUATION = 0x00;
constexpr int WS_OPCODE_TEXT         = 0x01;

enum class Kind : std::uint8_t { Websocket, Mqtt };

struct Entry {
    explicit Entry(const StreamPolicy &policy) : core(policy) {}

    const char    *name = "";
    Kind           kind = Kind::Websocket;
    StreamCore     core;
    StreamHandlers on;

    esp_websocket_client_handle_t ws = nullptr;
    std::string                   uri;
    bool                          assemble    = true;
    std::size_t                   max_message = 0;
    std::string                   rx;  // the message coming in, on the transport's task
    bool                          in_message = false;

    esp_mqtt_client_handle_t mqtt         = nullptr;
    bool                     mqtt_running = false;  // its task, which waits between connects
};

std::array<Entry *, MAX_STREAMS> s_streams{};
int                              s_count = 0;
SemaphoreHandle_t                s_lock  = nullptr;
TaskHandle_t                     s_task  = nullptr;

Entry *entry(Stream stream)
{
    return stream >= 0 && stream < s_count ? s_streams[stream] : nullptr;
}

std::int64_t now_us()
{
    return esp_timer_get_time();
}

template <typename F>
auto locked(F &&f)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if constexpr (std::is_void_v<decltype(f())>) {
        f();
        xSemaphoreGive(s_lock);
    } else {
        auto result = f();
        xSemaphoreGive(s_lock);
        return result;
    }
}

void wake()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

void opened(Entry &e)
{
    locked([&] { e.core.opened(now_us()); });
    ESP_LOGI(e.name, "open");
    if (e.on.opened) {
        e.on.opened();
    }
    wake();
}

void closed(Entry &e, const char *why)
{
    const bool was = locked([&] {
        const bool live = e.core.state() == StreamState::Open || e.core.state() == StreamState::Ready ||
                          e.core.state() == StreamState::Connecting;
        e.core.closed(now_us(), why);
        return live;
    });
    if (was) {
        const StreamStatus status = locked([&] { return e.core.status(now_us()); });
        ESP_LOGW(e.name, "%s, again in %d s", why, (status.next_in_ms + 999) / 1000);
    }
    if (e.on.closed) {
        e.on.closed();
    }
    wake();
}

void take_data(Entry &e, const esp_websocket_event_data_t *data)
{
    const bool first = data->op_code == WS_OPCODE_TEXT && data->payload_offset == 0;
    if (first) {
        e.in_message = true;
        e.rx.clear();
    } else if (data->op_code != WS_OPCODE_CONTINUATION && data->op_code != WS_OPCODE_TEXT) {
        return;  // binary, ping, pong, close: not text
    }
    if (!e.in_message) {
        return;
    }
    const auto length = static_cast<std::size_t>(std::max(data->data_len, 0));
    const bool last   = data->payload_offset + data->data_len >= data->payload_len && data->fin;
    if (!e.assemble) {
        if (e.on.fragment) {
            e.on.fragment(Fragment{data->data_ptr, length, first, last});
        }
        e.in_message = !last;
        return;
    }
    if (e.rx.size() + length > e.max_message) {
        ESP_LOGW(e.name, "message over %u bytes, dropping", static_cast<unsigned>(e.max_message));
        e.rx.clear();
        e.in_message = false;
        return;
    }
    e.rx.append(data->data_ptr, length);
    if (last) {
        e.in_message = false;
        if (e.on.message) {
            e.on.message(e.rx);
        }
        e.rx.clear();
    }
}

void on_websocket(void *arg, esp_event_base_t, std::int32_t id, void *data)
{
    Entry &e = *static_cast<Entry *>(arg);
    switch (static_cast<esp_websocket_event_id_t>(id)) {
        case WEBSOCKET_EVENT_CONNECTED:
            e.rx.clear();
            e.in_message = false;
            opened(e);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            e.rx.clear();
            e.in_message = false;
            closed(e, "disconnected");
            break;
        case WEBSOCKET_EVENT_CLOSED:
            e.rx.clear();
            e.in_message = false;
            closed(e, "closed by the other end");
            break;
        case WEBSOCKET_EVENT_DATA:
            take_data(e, static_cast<const esp_websocket_event_data_t *>(data));
            break;
        default:
            break;
    }
}

void on_mqtt(void *arg, esp_event_base_t, std::int32_t id, void *data)
{
    Entry &e = *static_cast<Entry *>(arg);
    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED: opened(e); break;
        case MQTT_EVENT_DISCONNECTED: closed(e, "disconnected"); break;
        default: break;
    }
    if (e.on.mqtt) {
        e.on.mqtt(static_cast<esp_mqtt_event_handle_t>(data));
    }
}

bool send_on(Entry &e, const std::string &text)
{
    return e.ws != nullptr && !text.empty() && esp_websocket_client_is_connected(e.ws) &&
           esp_websocket_client_send_text(e.ws, text.data(), static_cast<int>(text.size()), SEND_TIMEOUT) >= 0;
}

esp_err_t start_transport(Entry &e)
{
    if (e.kind == Kind::Websocket) {
        return esp_websocket_client_start(e.ws);
    }
    // Between connects its task waits to be asked; once stopped it is started.
    if (e.mqtt_running && esp_mqtt_client_reconnect(e.mqtt) == ESP_OK) {
        return ESP_OK;
    }
    if (e.mqtt_running) {
        esp_mqtt_client_stop(e.mqtt);
    }
    const esp_err_t err = esp_mqtt_client_start(e.mqtt);
    e.mqtt_running      = err == ESP_OK;
    return err;
}

void stop_transport(Entry &e)
{
    if (e.kind == Kind::Websocket) {
        esp_websocket_client_stop(e.ws);
    } else if (e.mqtt_running) {
        esp_mqtt_client_stop(e.mqtt);
        e.mqtt_running = false;
    }
}

void act(Entry &e, StreamAction action)
{
    switch (action) {
        case StreamAction::Start:
            ESP_LOGI(e.name, "connecting");
            if (const esp_err_t err = start_transport(e); err != ESP_OK) {
                closed(e, esp_err_to_name(err));
            }
            break;
        case StreamAction::Stop: {
            const StreamStatus status = locked([&] { return e.core.status(now_us()); });
            if (status.state == StreamState::Waiting && !status.last_error.empty()) {
                ESP_LOGW(e.name, "%s, again in %d s", status.last_error.c_str(),
                         (status.next_in_ms + 999) / 1000);
            }
            stop_transport(e);
            break;
        }
        case StreamAction::KeepAlive:
            if (e.on.keep_alive) {
                const std::string text = e.on.keep_alive();
                send_on(e, text);
            }
            break;
        case StreamAction::None: break;
    }
}

[[noreturn]] void stream_task(void *)
{
    for (;;) {
        const bool   online  = app::get(app::Fact::Online);
        std::int64_t now     = now_us();
        std::int64_t soonest = now + LOOK_EVERY_US;
        const int    count   = locked([] { return s_count; });
        for (int i = 0; i < count; ++i) {
            Entry &e = *s_streams[i];
            for (;;) {
                now                       = now_us();
                const StreamAction action = locked([&] {
                    e.core.network(online, now);
                    return e.core.next(now);
                });
                if (action == StreamAction::None) {
                    break;
                }
                act(e, action);
            }
            soonest = std::min(soonest, locked([&] { return e.core.due(now); }));
        }
        const std::int64_t wait_us = std::max(soonest - now_us(), SOONEST_US);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_us / 1000));
    }
}

esp_err_t start_task()
{
    if (s_task != nullptr) {
        return ESP_OK;
    }
    static StaticSemaphore_t lock_ctrl;
    s_lock = xSemaphoreCreateMutexStatic(&lock_ctrl);
    static StaticTask_t ctrl;
    auto *stack = static_cast<StackType_t *>(
        heap_caps_malloc(TASK_STACK * sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stack == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    s_task = xTaskCreateStaticPinnedToCore(stream_task, "net_streams", TASK_STACK, nullptr, TASK_PRIORITY,
                                           stack, &ctrl, TASK_CORE);
    app::watch(app::Fact::Online, [](bool) { wake(); });  // back at once when the network is
    return s_task != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t add(Entry *e, Stream &into)
{
    into = kNoStream;
    if (start_task() != ESP_OK) {
        ESP_LOGE(e->name, "no task for streams");
        return ESP_ERR_NO_MEM;
    }
    // Set under the lock the task takes to find it, so before it can connect.
    const bool added = locked([&] {
        if (s_count == MAX_STREAMS) {
            return false;
        }
        into               = static_cast<Stream>(s_count);
        s_streams[s_count] = e;
        ++s_count;
        return true;
    });
    if (!added) {
        ESP_LOGE(e->name, "more streams than there is room for");
        return ESP_ERR_NO_MEM;
    }
    wake();
    return ESP_OK;
}
}  // namespace

esp_err_t open_websocket(Stream &into, const WebsocketConfig &config, StreamHandlers handlers)
{
    auto *e        = new Entry(config.policy);
    e->name        = config.name;
    e->kind        = Kind::Websocket;
    e->on          = std::move(handlers);
    e->uri         = config.uri;
    e->assemble    = config.assemble;
    e->max_message = config.max_message;

    esp_websocket_client_config_t cfg{};
    cfg.uri                    = e->uri.c_str();
    cfg.task_stack             = config.task_stack;
    cfg.buffer_size            = config.buffer_size;
    cfg.network_timeout_ms     = config.network_timeout_ms;
    cfg.disable_auto_reconnect = true;
    if (config.ping_interval_s > 0) {
        cfg.ping_interval_sec = config.ping_interval_s;
    }
    if (config.pingpong_timeout_s > 0) {
        cfg.pingpong_timeout_sec = config.pingpong_timeout_s;
    }
    if (config.tls_bundle) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    e->ws = esp_websocket_client_init(&cfg);
    if (e->ws == nullptr ||
        esp_websocket_register_events(e->ws, WEBSOCKET_EVENT_ANY, on_websocket, e) != ESP_OK) {
        ESP_LOGE(config.name, "websocket would not set up");
        into = kNoStream;
        return ESP_FAIL;
    }
    return add(e, into);
}

esp_err_t open_mqtt(Stream &into, const MqttConfig &config, StreamHandlers handlers)
{
    auto *e = new Entry(config.policy);
    e->name = config.name;
    e->kind = Kind::Mqtt;
    e->on   = std::move(handlers);

    esp_mqtt_client_config_t cfg          = config.client;
    cfg.network.disable_auto_reconnect    = true;
    e->mqtt = esp_mqtt_client_init(&cfg);
    if (e->mqtt == nullptr || esp_mqtt_client_register_event(e->mqtt, MQTT_EVENT_ANY, on_mqtt, e) != ESP_OK) {
        ESP_LOGE(config.name, "MQTT would not set up");
        into = kNoStream;
        return ESP_FAIL;
    }
    return add(e, into);
}

bool stream_send(Stream stream, const std::string &text)
{
    Entry *e = entry(stream);
    return e != nullptr && send_on(*e, text);
}

esp_mqtt_client_handle_t stream_mqtt(Stream stream)
{
    Entry *e = entry(stream);
    return e != nullptr ? e->mqtt : nullptr;
}

void stream_ready(Stream stream)
{
    if (Entry *e = entry(stream); e != nullptr) {
        locked([&] { e->core.ready(now_us()); });
        ESP_LOGI(e->name, "ready");
        wake();
    }
}

void stream_fail(Stream stream, const char *why, int retry_ms)
{
    if (Entry *e = entry(stream); e != nullptr) {
        locked([&] { e->core.fail(now_us(), why, retry_ms); });
        wake();
    }
}

void stream_restart(Stream stream)
{
    if (Entry *e = entry(stream); e != nullptr) {
        locked([&] { e->core.restart(now_us()); });
        ESP_LOGI(e->name, "restart asked for");
        wake();
    }
}

bool stream_is_ready(Stream stream)
{
    Entry *e = entry(stream);
    return e != nullptr && locked([&] { return e->core.state() == StreamState::Ready; });
}

int stream_count()
{
    return s_lock == nullptr ? 0 : locked([] { return s_count; });
}

Stream find_stream(const char *name)
{
    for (int i = 0; i < stream_count(); ++i) {
        if (std::strcmp(s_streams[i]->name, name) == 0) {
            return i;
        }
    }
    return kNoStream;
}

const char *stream_name(Stream stream)
{
    Entry *e = entry(stream);
    return e != nullptr ? e->name : "";
}

StreamStatus stream_status(Stream stream)
{
    Entry *e = entry(stream);
    return e != nullptr ? locked([&] { return e->core.status(now_us()); }) : StreamStatus{};
}

}  // namespace net
