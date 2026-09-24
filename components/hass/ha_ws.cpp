#include "ha_ws.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <cstring>
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

constexpr int BUFFER_SIZE = 4096;
constexpr int TASK_STACK = 6144;  // measured: uses 2.8 KB

constexpr std::size_t MAX_MESSAGE = 256 * 1024;

constexpr int SUBSCRIBE_ID = 2;

esp_websocket_client_handle_t s_client = nullptr;
std::atomic<bool>             s_connected{false};

std::atomic<int> s_next_command_id{SUBSCRIBE_ID + 1};

int next_command_id()
{
    return s_next_command_id.fetch_add(1, std::memory_order_relaxed);
}
std::atomic<bool>       s_stop_wanted{false};
std::atomic<TickType_t> s_start_at{0};  // zero when no start is pending

constexpr TickType_t RETRY_SUBSCRIBE = pdMS_TO_TICKS(10000);
constexpr TickType_t RETRY_TOKEN     = pdMS_TO_TICKS(60000);

/** The client's own reconnect only covers the socket; a refusal from the other
 *  end needs the session torn down and begun again, after a pause. */
void begin_again_in(TickType_t delay, const char *why)
{
    ESP_LOGW(TAG, "%s, beginning again in %u s", why,
             static_cast<unsigned>(pdTICKS_TO_MS(delay) / 1000));
    s_connected.store(false, std::memory_order_relaxed);
    s_stop_wanted.store(true, std::memory_order_relaxed);
    TickType_t at = xTaskGetTickCount() + delay;
    if (at == 0) {
        at = 1;
    }
    s_start_at.store(at, std::memory_order_relaxed);
}

struct Command {
    char domain[16];
    char service[40];
    char entity[96];
    char field[32];
    char value[32];
    bool with_field;
};

constexpr UBaseType_t COMMAND_QUEUE_LEN = 8;
StaticQueue_t s_command_queue_ctrl;
Command       s_command_queue_storage[COMMAND_QUEUE_LEN];
QueueHandle_t s_commands = nullptr;

void copy_into(char *dest, std::size_t size, const char *source)
{
    std::strncpy(dest, source != nullptr ? source : "", size);
    dest[size - 1] = '\0';
}

esp_err_t enqueue(const char *domain, const char *service, const char *entity_id,
                  const char *field, const char *value)
{
    ESP_RETURN_ON_FALSE(s_commands != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");

    Command cmd{};
    copy_into(cmd.domain, sizeof(cmd.domain), domain);
    copy_into(cmd.service, sizeof(cmd.service), service);
    copy_into(cmd.entity, sizeof(cmd.entity), entity_id);
    cmd.with_field = field != nullptr;
    if (cmd.with_field) {
        copy_into(cmd.field, sizeof(cmd.field), field);
        copy_into(cmd.value, sizeof(cmd.value), value);
    }
    ESP_RETURN_ON_FALSE(xQueueSend(s_commands, &cmd, 0) == pdTRUE, ESP_ERR_NO_MEM, TAG,
                        "command queue full");
    return ESP_OK;
}

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
                s_next_command_id.store(SUBSCRIBE_ID + 1, std::memory_order_relaxed);
                send_text(subscribe);
                s_connected.store(true, std::memory_order_relaxed);
            }
            break;
        }

        case MessageType::AuthInvalid:
            begin_again_in(RETRY_TOKEN, "token rejected");
            break;

        case MessageType::Event: {
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
            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "success");
            if (cJSON_IsFalse(ok)) {
                const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
                const cJSON *msg =
                    cJSON_GetObjectItemCaseSensitive(error, "message");
                ESP_LOGW(TAG, "command %d refused: %s", message_id(root),
                         cJSON_IsString(msg) ? msg->valuestring : "unknown");
                if (message_id(root) == SUBSCRIBE_ID) {
                    begin_again_in(RETRY_SUBSCRIBE, "subscription refused");
                }
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
    switch (event->op_code) {
        case 0x01:  // text: first or only frame
            if (event->payload_offset == 0) {
                s_rx.clear();
            }
            break;
        case 0x00:  // continuation of the message in flight
            if (s_rx.empty()) {
                return;
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
        return;
    }
    if (!event->fin) {
        return;
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
                ESP_LOGW(TAG, "%s, client retries on its own",
                         id == WEBSOCKET_EVENT_CLOSED ? "closed by the server" : "disconnected");
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
        Command cmd;
        if (xQueueReceive(s_commands, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (connected()) {
                send_text(cmd.with_field
                              ? call_service_message(next_command_id(), cmd.domain, cmd.service,
                                                     cmd.entity, cmd.field, cmd.value)
                              : call_service_message(next_command_id(), cmd.domain, cmd.service,
                                                     cmd.entity));
            } else {
                ESP_LOGW(TAG, "dropped %s.%s, not connected", cmd.domain, cmd.service);
            }
        }

        if (s_client == nullptr) {
            continue;
        }
        if (s_stop_wanted.exchange(false, std::memory_order_relaxed)) {
            esp_websocket_client_stop(s_client);
        }
        const TickType_t start_at = s_start_at.load(std::memory_order_relaxed);
        if (start_at != 0 &&
            static_cast<std::int32_t>(xTaskGetTickCount() - start_at) >= 0) {
            s_start_at.store(0, std::memory_order_relaxed);
            ESP_LOGI(TAG, "connecting again");
            esp_websocket_client_start(s_client);
        }
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
    // Home Assistant closes every socket cleanly when it restarts, and without
    // this the client treats a clean close as the end and exits for good.
    cfg.enable_close_reconnect = true;

    s_client = esp_websocket_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != nullptr, ESP_FAIL, TAG, "init");
    ESP_RETURN_ON_ERROR(
        esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_event, nullptr), TAG,
        "events");

    static StaticTask_t task_ctrl;
    static StackType_t  task_stack[6144];
    s_commands = xQueueCreateStatic(COMMAND_QUEUE_LEN, sizeof(Command),
                                    reinterpret_cast<std::uint8_t *>(s_command_queue_storage),
                                    &s_command_queue_ctrl);
    ESP_RETURN_ON_FALSE(s_commands != nullptr, ESP_ERR_NO_MEM, TAG, "command queue");

    xTaskCreateStaticPinnedToCore(supervisor_task, "ha_ws_sup", sizeof(task_stack), nullptr, 2,
                                  task_stack, &task_ctrl, 0);

    ESP_LOGI(TAG, "connecting to %s", HASS_WS_URI);
    return esp_websocket_client_start(s_client);
}

bool connected()
{
    return s_connected.load(std::memory_order_relaxed);
}

esp_err_t restart()
{
    if (s_client == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    begin_again_in(1, "restart asked for");
    return ESP_OK;
}

esp_err_t call_service(const char *domain, const char *service, const char *entity_id)
{
    ESP_RETURN_ON_FALSE(connected(), ESP_ERR_INVALID_STATE, TAG, "not connected");
    return enqueue(domain, service, entity_id, nullptr, nullptr);
}

esp_err_t call_service_with(const char *domain, const char *service, const char *entity_id,
                            const char *field, const char *value)
{
    ESP_RETURN_ON_FALSE(connected(), ESP_ERR_INVALID_STATE, TAG, "not connected");
    return enqueue(domain, service, entity_id, field, value);
}

}  // namespace hass::ws
