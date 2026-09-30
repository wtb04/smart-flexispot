#include "ha_ws.h"

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_stream.h"
#include "units.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if __has_include("hass_secrets.h")
#include "hass_secrets.h"
#endif
#ifndef HASS_WS_URI
#define HASS_WS_URI ""
#define HASS_WS_TOKEN ""
#endif

namespace hass::ws {
namespace {
constexpr char TAG[] = "ha_ws";

constexpr int BUFFER_SIZE = 4 * units::kBytesPerKiB;
constexpr int TASK_STACK  = 6144;  // measured: uses 2.8 KB

constexpr int NETWORK_TIMEOUT_MS = 10 * units::kMsPerSecond;
constexpr int PING_INTERVAL_S    = 25;
constexpr int PINGPONG_TIMEOUT_S = 90;

constexpr std::size_t MAX_MESSAGE = 256 * units::kBytesPerKiB;

constexpr int SUBSCRIBE_ID = 2;

constexpr TickType_t COMMAND_WAIT    = pdMS_TO_TICKS(500);
constexpr int        RETRY_SEND_MS   = 5 * units::kMsPerSecond;
constexpr int        RETRY_REFUSE_MS = 10 * units::kMsPerSecond;
constexpr int        RETRY_TOKEN_MS  = units::kMsPerMinute;

constexpr std::size_t SUPERVISOR_STACK    = 6144;
constexpr UBaseType_t SUPERVISOR_PRIORITY = 2;
constexpr BaseType_t  SUPERVISOR_CORE     = 0;

constexpr std::size_t DOMAIN_SIZE  = 16;
constexpr std::size_t SERVICE_SIZE = 40;
constexpr std::size_t ENTITY_SIZE  = 96;
constexpr std::size_t FIELD_SIZE   = 32;
constexpr std::size_t VALUE_SIZE   = 32;
constexpr std::size_t REFUSAL_SIZE = 64;
constexpr std::size_t ORIGIN_SIZE  = 96;
constexpr std::size_t REQUEST_SIZE = 384;

constexpr char SCHEME_SEPARATOR[] = "://";
constexpr char SECURE_SCHEME[]    = "wss";

net::Stream s_stream = net::kNoStream;

std::atomic<int> s_next_command_id{SUBSCRIBE_ID + 1};

int next_command_id()
{
    return s_next_command_id.fetch_add(1, std::memory_order_relaxed);
}

// Sleeps between rounds of its own, and is woken for anything to send.
TaskHandle_t s_supervisor = nullptr;

void wake_supervisor()
{
    if (s_supervisor != nullptr) {
        xTaskNotifyGive(s_supervisor);
    }
}

struct Command {
    char domain[DOMAIN_SIZE];
    char service[SERVICE_SIZE];
    char entity[ENTITY_SIZE];
    char field[FIELD_SIZE];
    char value[VALUE_SIZE];
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
    wake_supervisor();
    return ESP_OK;
}

// Requests wait in PSRAM: each is several hundred bytes, and they are rare.
struct Request {
    char         body[REQUEST_SIZE];
    ReplyHandler on_reply;
};

constexpr UBaseType_t REQUEST_QUEUE_LEN = 4;
StaticQueue_t         s_request_queue_ctrl;
QueueHandle_t         s_requests = nullptr;

// Sent and not yet answered, by the number each went out with.
struct Pending {
    int          id;
    ReplyHandler on_reply;
};

constexpr int PENDING_MAX = 4;
Pending       s_pending[PENDING_MAX]{};
portMUX_TYPE  s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

bool await_reply(int id, ReplyHandler on_reply)
{
    portENTER_CRITICAL(&s_pending_lock);
    for (Pending &slot : s_pending) {
        if (slot.on_reply == nullptr) {
            slot = {id, on_reply};
            portEXIT_CRITICAL(&s_pending_lock);
            return true;
        }
    }
    portEXIT_CRITICAL(&s_pending_lock);
    return false;
}

ReplyHandler take_awaited(int id)
{
    portENTER_CRITICAL(&s_pending_lock);
    for (Pending &slot : s_pending) {
        if (slot.on_reply != nullptr && slot.id == id) {
            const ReplyHandler on_reply = slot.on_reply;
            slot                        = {};
            portEXIT_CRITICAL(&s_pending_lock);
            return on_reply;
        }
    }
    portEXIT_CRITICAL(&s_pending_lock);
    return nullptr;
}

/** Nothing sent before a reconnect will be answered after it. */
void fail_awaited()
{
    Pending failed[PENDING_MAX];
    portENTER_CRITICAL(&s_pending_lock);
    std::memcpy(failed, s_pending, sizeof(failed));
    std::memset(s_pending, 0, sizeof(s_pending));
    portEXIT_CRITICAL(&s_pending_lock);
    for (const Pending &slot : failed) {
        if (slot.on_reply != nullptr) {
            slot.on_reply(nullptr);
        }
    }
}

/** True when the reply was to a request, which is then answered. */
bool answer_request(const cJSON *root)
{
    const ReplyHandler on_reply = take_awaited(message_id(root));
    if (on_reply == nullptr) {
        return false;
    }
    const cJSON *result = reply_result(root);
    if (result == nullptr) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON *msg   = cJSON_GetObjectItemCaseSensitive(error, "message");
        ESP_LOGW(TAG, "request %d refused: %s", message_id(root),
                 cJSON_IsString(msg) ? msg->valuestring : "unknown");
    }
    on_reply(result);
    return true;
}

EntityStore    s_store;
UpdateHandler  s_on_update  = nullptr;
RefusalHandler s_on_refusal = nullptr;

portMUX_TYPE s_refusal_lock          = portMUX_INITIALIZER_UNLOCKED;
char         s_refusal[REFUSAL_SIZE] = {};
std::int64_t s_refused_at            = 0;

void refused(const char *reason)
{
    portENTER_CRITICAL(&s_refusal_lock);
    std::size_t i = 0;
    for (; reason != nullptr && reason[i] != '\0' && i + 1 < sizeof(s_refusal); ++i) {
        s_refusal[i] = reason[i];
    }
    s_refusal[i]  = '\0';
    s_refused_at  = esp_timer_get_time();
    portEXIT_CRITICAL(&s_refusal_lock);
    if (s_on_refusal != nullptr) {
        s_on_refusal(reason);
    }
}

std::vector<std::string> s_entities;

bool send_text(const std::string &text)
{
    return net::stream_send(s_stream, text);
}

void subscribe()
{
    const std::vector<std::string> &entities = s_entities;
    ESP_LOGI(TAG, "authenticated, subscribing to %u entities",
             static_cast<unsigned>(entities.size()));
    const std::string subscribe = subscribe_entities_message(SUBSCRIBE_ID, entities);
    if (subscribe.empty()) {
        ESP_LOGW(TAG, "no entities to subscribe to");
        return;
    }
    fail_awaited();  // numbering starts again
    s_next_command_id.store(SUBSCRIBE_ID + 1, std::memory_order_relaxed);
    if (send_text(subscribe)) {
        net::stream_ready(s_stream);
    } else {
        net::stream_fail(s_stream, "could not subscribe", RETRY_SEND_MS);
    }
}

void apply_event(const cJSON *root)
{
    if (message_id(root) != SUBSCRIBE_ID) {
        return;
    }
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    if (s_store.apply_event(event) && s_on_update != nullptr) {
        s_on_update(s_store);
    }
}

void check_result(const cJSON *root)
{
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (!cJSON_IsFalse(ok)) {
        return;
    }
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON *msg   = cJSON_GetObjectItemCaseSensitive(error, "message");
    const char  *why   = cJSON_IsString(msg) ? msg->valuestring : "unknown";
    ESP_LOGW(TAG, "command %d refused: %s", message_id(root), why);
    if (message_id(root) == SUBSCRIBE_ID) {
        net::stream_fail(s_stream, "subscription refused", RETRY_REFUSE_MS);
    } else {
        refused(why);
    }
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
            if (!send_text(auth_message(HASS_WS_TOKEN))) {
                net::stream_fail(s_stream, "could not send the token", RETRY_SEND_MS);
            }
            break;
        case MessageType::AuthOk:      subscribe(); break;
        case MessageType::AuthInvalid: net::stream_fail(s_stream, "token rejected", RETRY_TOKEN_MS); break;
        case MessageType::Event:       apply_event(root); break;
        case MessageType::Result:
            if (!answer_request(root)) {
                check_result(root);
            }
            break;
        default:                       break;
    }

    cJSON_Delete(root);
}

void send_command(const Command &cmd)
{
    if (!connected()) {
        ESP_LOGW(TAG, "dropped %s.%s, not connected", cmd.domain, cmd.service);
        refused("not connected");
        return;
    }
    send_text(cmd.with_field ? call_service_message(next_command_id(), cmd.domain, cmd.service,
                                                    cmd.entity, cmd.field, cmd.value)
                             : call_service_message(next_command_id(), cmd.domain, cmd.service,
                                                    cmd.entity));
}

void send_request(const Request &req)
{
    const int id = next_command_id();
    if (!connected() || !await_reply(id, req.on_reply)) {
        ESP_LOGW(TAG, "request not sent: %s", connected() ? "too many waiting" : "not connected");
        req.on_reply(nullptr);
        return;
    }
    const std::string text = numbered(id, req.body);
    if (!send_text(text)) {
        if (const ReplyHandler on_reply = take_awaited(id); on_reply != nullptr) {
            on_reply(nullptr);
        }
    }
}

[[noreturn]] void supervisor_task(void *)
{
    for (;;) {
        Command cmd;
        ulTaskNotifyTake(pdTRUE, COMMAND_WAIT);
        while (xQueueReceive(s_commands, &cmd, 0) == pdTRUE) {
            send_command(cmd);
        }
        for (Request req; xQueueReceive(s_requests, &req, 0) == pdTRUE;) {
            send_request(req);
        }
    }
}

}  // namespace

esp_err_t start(UpdateHandler on_update, std::vector<std::string> entities,
                std::vector<std::string> attributes)
{
    s_entities = std::move(entities);
    s_store.keep_attributes(std::move(attributes));
    if (std::string(HASS_WS_URI).empty()) {
        ESP_LOGW(TAG, "no Home Assistant URL configured, not starting");
        return ESP_OK;
    }
    s_on_update = on_update;
    static StaticTask_t task_ctrl;
    static StackType_t  task_stack[SUPERVISOR_STACK];
    s_commands = xQueueCreateStatic(COMMAND_QUEUE_LEN, sizeof(Command),
                                    reinterpret_cast<std::uint8_t *>(s_command_queue_storage),
                                    &s_command_queue_ctrl);
    ESP_RETURN_ON_FALSE(s_commands != nullptr, ESP_ERR_NO_MEM, TAG, "command queue");
    auto *request_storage = static_cast<std::uint8_t *>(heap_caps_malloc(
        REQUEST_QUEUE_LEN * sizeof(Request), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_RETURN_ON_FALSE(request_storage != nullptr, ESP_ERR_NO_MEM, TAG, "request storage");
    s_requests = xQueueCreateStatic(REQUEST_QUEUE_LEN, sizeof(Request), request_storage,
                                    &s_request_queue_ctrl);
    ESP_RETURN_ON_FALSE(s_requests != nullptr, ESP_ERR_NO_MEM, TAG, "request queue");

    s_supervisor =
        xTaskCreateStaticPinnedToCore(supervisor_task, "ha_ws_sup", sizeof(task_stack), nullptr,
                                      SUPERVISOR_PRIORITY, task_stack, &task_ctrl, SUPERVISOR_CORE);

    net::WebsocketConfig config;
    config.name               = TAG;
    config.uri                = HASS_WS_URI;
    config.task_stack         = TASK_STACK;
    config.buffer_size        = BUFFER_SIZE;
    config.network_timeout_ms = NETWORK_TIMEOUT_MS;
    config.ping_interval_s    = PING_INTERVAL_S;
    config.pingpong_timeout_s = PINGPONG_TIMEOUT_S;
    config.max_message        = MAX_MESSAGE;

    net::StreamHandlers on;
    on.opened  = [] { ESP_LOGI(TAG, "socket open, waiting for auth_required"); };
    on.closed  = fail_awaited;  // nothing sent before will be answered after
    on.message = handle_message;
    return net::open_websocket(s_stream, config, std::move(on));
}

bool connected()
{
    return net::stream_is_ready(s_stream);
}

const char *http_origin()
{
    static char origin[ORIGIN_SIZE];
    if (origin[0] != '\0') {
        return origin;
    }
    const char *uri  = HASS_WS_URI;
    const char *host = std::strstr(uri, SCHEME_SEPARATOR);
    host             = host != nullptr ? host + std::strlen(SCHEME_SEPARATOR) : uri;
    const char       *end = std::strchr(host, '/');
    const std::size_t len = end != nullptr ? static_cast<std::size_t>(end - host) : std::strlen(host);

    const bool secure = std::strncmp(uri, SECURE_SCHEME, std::strlen(SECURE_SCHEME)) == 0;
    if (len > 0) {
        std::snprintf(origin, sizeof(origin), "%s%.*s", secure ? "https://" : "http://",
                      static_cast<int>(len), host);
    }
    return origin;
}

bool last_refusal(char *out, std::size_t size, int &age_s)
{
    if (size == 0) {
        return false;
    }
    portENTER_CRITICAL(&s_refusal_lock);
    const std::int64_t at = s_refused_at;
    std::size_t        i  = 0;
    for (; s_refusal[i] != '\0' && i + 1 < size; ++i) {
        out[i] = s_refusal[i];
    }
    out[i] = '\0';
    portEXIT_CRITICAL(&s_refusal_lock);
    age_s = static_cast<int>((esp_timer_get_time() - at) / units::kUsPerSecond);
    return at != 0;
}

void on_refusal(RefusalHandler handler)
{
    s_on_refusal = handler;
}

esp_err_t call_service(const char *domain, const char *service, const char *entity_id)
{
    return call_service_with(domain, service, entity_id, nullptr, nullptr);
}

esp_err_t call_service_with(const char *domain, const char *service, const char *entity_id,
                            const char *field, const char *value)
{
    if (!connected()) {
        refused("not connected");
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = enqueue(domain, service, entity_id, field, value);
    if (err != ESP_OK) {
        refused("too many at once");
    }
    return err;
}

esp_err_t request(const std::string &body, ReplyHandler on_reply)
{
    ESP_RETURN_ON_FALSE(on_reply != nullptr, ESP_ERR_INVALID_ARG, TAG, "no reply handler");
    ESP_RETURN_ON_FALSE(body.size() < REQUEST_SIZE, ESP_ERR_INVALID_SIZE, TAG, "request too long");
    if (s_requests == nullptr || !connected()) {
        on_reply(nullptr);
        return ESP_ERR_INVALID_STATE;
    }
    // Too large for a caller's stack, so one is shared; the queue copies it.
    static Request           req;
    static StaticSemaphore_t lock_ctrl;
    static SemaphoreHandle_t lock = xSemaphoreCreateMutexStatic(&lock_ctrl);
    xSemaphoreTake(lock, portMAX_DELAY);
    copy_into(req.body, sizeof(req.body), body.c_str());
    req.on_reply   = on_reply;
    const bool put = xQueueSend(s_requests, &req, 0) == pdTRUE;
    xSemaphoreGive(lock);
    wake_supervisor();
    if (!put) {
        on_reply(nullptr);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace hass::ws
