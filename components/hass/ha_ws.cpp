#include "ha_ws.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <string>
#include <vector>

#if __has_include("hass_secrets.h")
#include "hass_secrets.h"
#endif
#ifndef HASS_WS_URI
#define HASS_WS_URI ""
#define HASS_WS_TOKEN ""
#define HASS_WS_ENTITIES ""
#endif

namespace hass::ws {
namespace {

constexpr char TAG[] = "ha_ws";

// 4 KB of receive buffer means a large state dump arrives as several events
// rather than one, which is fine -- it costs twice this in internal heap, and
// the buffer size does not cap message size.
constexpr int BUFFER_SIZE = 4096;
// The 4 KB default overflows when the client formats a connection error, which
// happens on the first attempt while Wi-Fi is still associating.
constexpr int TASK_STACK = 8192;

// A hostile or confused server could otherwise walk us into heap exhaustion:
// nothing in the client caps reassembly.
constexpr std::size_t MAX_MESSAGE = 256 * 1024;

constexpr int SUBSCRIBE_ID = 2;

esp_websocket_client_handle_t s_client = nullptr;
std::atomic<bool>             s_connected{false};

// Home Assistant requires every command id to be larger than any id already
// used on the connection, and answers anything else with an error instead of
// running the service. One counter for all commands: separate per-call-site
// counters meant a setpoint (high id) permanently blocked every later toggle
// (low id), which looked like the buttons had stopped working.
std::atomic<int> s_next_command_id{SUBSCRIBE_ID + 1};

int next_command_id()
{
    return s_next_command_id.fetch_add(1, std::memory_order_relaxed);
}
// Set from the socket task, acted on elsewhere: stopping or destroying the
// client from its own event handler frees the running task's control block.
std::atomic<bool>             s_shutdown_wanted{false};

std::string   s_rx;
EntityStore   s_store;
UpdateHandler s_on_update = nullptr;

std::vector<std::string> configured_entities()
{
    std::vector<std::string> out;
    std::string              list = HASS_WS_ENTITIES;
    std::size_t              start = 0;
    while (start < list.size()) {
        std::size_t comma = list.find(',', start);
        if (comma == std::string::npos) {
            comma = list.size();
        }
        std::string id = list.substr(start, comma - start);
        // Tolerate spaces after commas in the secrets header.
        while (!id.empty() && id.front() == ' ') id.erase(id.begin());
        while (!id.empty() && id.back() == ' ') id.pop_back();
        if (!id.empty()) {
            out.push_back(id);
        }
        start = comma + 1;
    }
    return out;
}

void send_text(const std::string &text)
{
    if (s_client == nullptr || text.empty()) {
        return;
    }
    // A finite timeout: portMAX_DELAY here would stall the read loop on a
    // wedged socket. Sending from the handler is safe -- the client's lock is
    // recursive and held by this same task.
    esp_websocket_client_send_text(s_client, text.data(), static_cast<int>(text.size()),
                                   pdMS_TO_TICKS(5000));
}

void handle_message(const std::string &text)
{
    cJSON *root = cJSON_ParseWithLength(text.data(), text.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "unparseable message (%u bytes)", static_cast<unsigned>(text.size()));
        return;
    }

    switch (classify(root)) {
        case MessageType::AuthRequired:
            send_text(auth_message(HASS_WS_TOKEN));
            break;

        case MessageType::AuthOk: {
            const std::vector<std::string> entities = configured_entities();
            ESP_LOGI(TAG, "authenticated, subscribing to %u entities",
                     static_cast<unsigned>(entities.size()));
            const std::string subscribe = subscribe_entities_message(SUBSCRIBE_ID, entities);
            if (subscribe.empty()) {
                ESP_LOGW(TAG, "no entities configured - see hass_secrets.example.h");
            } else {
                // Ids are per connection, so a reconnect starts over.
                s_next_command_id.store(SUBSCRIBE_ID + 1, std::memory_order_relaxed);
                send_text(subscribe);
                s_connected.store(true, std::memory_order_relaxed);
            }
            break;
        }

        case MessageType::AuthInvalid:
            // A bad token cannot fix itself, and the client would otherwise
            // reconnect every few seconds forever, filling Home Assistant's log.
            ESP_LOGE(TAG, "token rejected, not retrying");
            s_shutdown_wanted.store(true, std::memory_order_relaxed);
            break;

        case MessageType::Event: {
            // Gate on the subscription id: a second subscription would
            // otherwise feed the same store.
            if (message_id(root) != SUBSCRIBE_ID) {
                break;
            }
            const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
            if (s_store.apply_event(event) && s_on_update != nullptr) {
                s_on_update(s_store);
            }
            break;
        }

        case MessageType::Result: {
            // Only failures are worth a line, but they are worth it: a refused
            // command otherwise does nothing at all with no trace of why.
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "success");
            if (cJSON_IsFalse(ok)) {
                const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
                const cJSON *msg =
                    cJSON_GetObjectItemCaseSensitive(error, "message");
                ESP_LOGW(TAG, "command %d refused: %s", message_id(root),
                         cJSON_IsString(msg) ? msg->valuestring : "unknown");
            }
            break;
        }

        default:
            break;
    }

    cJSON_Delete(root);
}

void handle_data(const esp_websocket_event_data_t *event)
{
    // Two independent kinds of splitting. A frame larger than the buffer
    // arrives as several events with a rising payload_offset; a fragmented
    // message arrives as separate frames, where the offset restarts at zero and
    // the opcode is 0 for continuations. Terminating on payload_len alone
    // truncates the second case.
    switch (event->op_code) {
        case 0x01:  // text: first or only frame
            if (event->payload_offset == 0) {
                s_rx.clear();
            }
            break;
        case 0x00:  // continuation of the message in flight
            if (s_rx.empty()) {
                return;  // we missed the head; drop the message
            }
            break;
        default:
            return;  // binary, ping, pong, close: not ours
    }

    if (event->data_len > 0) {
        if (s_rx.size() + event->data_len > MAX_MESSAGE) {
            ESP_LOGW(TAG, "message over %u bytes, dropping",
                     static_cast<unsigned>(MAX_MESSAGE));
            s_rx.clear();
            return;
        }
        s_rx.append(event->data_ptr, static_cast<std::size_t>(event->data_len));
    }

    if (event->payload_offset + event->data_len < event->payload_len) {
        return;  // more of this frame is coming
    }
    if (!event->fin) {
        return;  // frame done, message continues
    }

    handle_message(s_rx);
    s_rx.clear();
}

void on_event(void *, esp_event_base_t, std::int32_t id, void *data)
{
    auto *event = static_cast<esp_websocket_event_data_t *>(data);

    switch (static_cast<esp_websocket_event_id_t>(id)) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "socket open, waiting for auth_required");
            s_rx.clear();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
            s_rx.clear();
            if (s_connected.exchange(false, std::memory_order_relaxed)) {
                ESP_LOGW(TAG, "disconnected, client retries on its own");
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            handle_data(event);
            break;
        default:
            break;
    }
}

[[noreturn]] void supervisor_task(void *)
{
    for (;;) {
        if (s_shutdown_wanted.exchange(false, std::memory_order_relaxed) && s_client != nullptr) {
            // Deliberately off the socket task: esp_websocket_client_destroy()
            // does not check that stopping succeeded and frees the client out
            // from under its own still-running task.
            esp_websocket_client_stop(s_client);
            ESP_LOGW(TAG, "client stopped");
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

esp_err_t start(UpdateHandler on_update)
{
    if (std::string(HASS_WS_URI).empty()) {
        ESP_LOGW(TAG, "no Home Assistant URL configured, not starting");
        return ESP_OK;
    }
    s_on_update = on_update;

    esp_websocket_client_config_t cfg = {};
    cfg.uri                  = HASS_WS_URI;
    cfg.task_stack           = TASK_STACK;
    cfg.buffer_size          = BUFFER_SIZE;
    cfg.network_timeout_ms   = 10000;
    cfg.reconnect_timeout_ms = 5000;
    cfg.ping_interval_sec    = 25;
    cfg.pingpong_timeout_sec = 90;

    s_client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != nullptr, ESP_FAIL, TAG, "init");
    ESP_RETURN_ON_ERROR(
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_event, nullptr), TAG,
        "events");

    static StaticTask_t task_ctrl;
    static StackType_t  task_stack[3072];
    xTaskCreateStaticPinnedToCore(supervisor_task, "ha_ws_sup", sizeof(task_stack), nullptr, 2,
                                  task_stack, &task_ctrl, 0);

    ESP_LOGI(TAG, "connecting to %s", HASS_WS_URI);
    return esp_websocket_client_start(s_client);
}

bool connected()
{
    return s_connected.load(std::memory_order_relaxed);
}

esp_err_t call_service(const char *domain, const char *service, const char *entity_id)
{
    ESP_RETURN_ON_FALSE(connected(), ESP_ERR_INVALID_STATE, TAG, "not connected");
    send_text(call_service_message(next_command_id(), domain, service, entity_id));
    return ESP_OK;
}

esp_err_t call_service_with(const char *domain, const char *service, const char *entity_id,
                            const char *field, const char *value)
{
    ESP_RETURN_ON_FALSE(connected(), ESP_ERR_INVALID_STATE, TAG, "not connected");
    send_text(call_service_message(next_command_id(), domain, service, entity_id, field, value));
    return ESP_OK;
}

}  // namespace hass::ws
