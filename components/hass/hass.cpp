#include "hass.h"

#include "esp_check.h"
#include "esp_log.h"
#include "mqtt_client.h"

#include <atomic>
#include <string>

#if __has_include("hass_secrets.h")
#include "hass_secrets.h"
#endif
#ifndef HASS_MQTT_URI
#define HASS_MQTT_URI ""
#define HASS_MQTT_USER ""
#define HASS_MQTT_PASSWORD ""
#define HASS_DEVICE_ID "tab5_desk"
#endif

namespace hass {
namespace {
constexpr char TAG[] = "hass";
constexpr char SW_VERSION[] = "1.0.0";

constexpr int QOS_AT_LEAST_ONCE = 1;
constexpr int RETAIN            = 1;

esp_mqtt_client_handle_t s_client = nullptr;
std::atomic<bool>        s_connected{false};
Handlers                 s_handlers{};
protocol::Topics         s_topics;
std::string              s_last_state;   // telemetry task only
std::atomic<bool>        s_force_publish{false};

struct Inbound {
    std::string topic;
    std::string payload;
};
Inbound s_inbound;

void publish_discovery()
{
    const std::string payload = protocol::discovery_document(HASS_DEVICE_ID, SW_VERSION);
    esp_mqtt_client_publish(s_client, s_topics.discovery.c_str(), payload.c_str(),
                            static_cast<int>(payload.size()), QOS_AT_LEAST_ONCE, RETAIN);
    ESP_LOGI(TAG, "published discovery (%u bytes)", static_cast<unsigned>(payload.size()));
}

void dispatch(const std::string &topic, const std::string &payload)
{
    if (topic == s_topics.cmd_preset) {
        const int preset = protocol::parse_preset(payload);
        if (preset > 0 && s_handlers.on_preset != nullptr) {
            s_handlers.on_preset(preset);
        }
        return;
    }
    if (topic == s_topics.cmd_brightness) {
        const int percent = protocol::parse_brightness(payload);
        if (percent >= 0 && s_handlers.on_brightness != nullptr) {
            s_handlers.on_brightness(percent);
        }
        return;
    }
    if (topic == s_topics.cmd_screen) {
        bool on = true;
        if (protocol::parse_screen(payload, on) && s_handlers.on_screen != nullptr) {
            s_handlers.on_screen(on);
        }
        return;
    }
    if (topic == s_topics.cmd_move) {
        const protocol::Move move = protocol::parse_move(payload);
        if (move != protocol::Move::Unknown && s_handlers.on_move != nullptr) {
            s_handlers.on_move(move);
        }
        return;
    }
    if (topic == s_topics.cmd_notify) {
        const protocol::Notification notice = protocol::parse_notification(payload);
        ESP_LOGI(TAG, "notify: valid=%d title='%s' message='%s'", notice.valid,
                 notice.title.c_str(), notice.message.c_str());
        if (notice.valid && s_handlers.on_notify != nullptr) {
            s_handlers.on_notify(notice);
        }
        return;
    }
    if (topic == "homeassistant/status" && payload == "online") {
        publish_discovery();
        s_force_publish.store(true, std::memory_order_relaxed);
    }
}

void on_mqtt_event(void *, esp_event_base_t, std::int32_t id, void *data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(data);

    switch (static_cast<esp_mqtt_event_id_t>(id)) {
        case MQTT_EVENT_CONNECTED:
            s_connected.store(true, std::memory_order_relaxed);
            ESP_LOGI(TAG, "connected to broker");
            esp_mqtt_client_publish(s_client, s_topics.availability.c_str(), "online", 0,
                                    QOS_AT_LEAST_ONCE, RETAIN);
            publish_discovery();
            s_force_publish.store(true, std::memory_order_relaxed);
            esp_mqtt_client_subscribe_single(s_client, s_topics.command.c_str(),
                                             QOS_AT_LEAST_ONCE);
            esp_mqtt_client_subscribe_single(s_client, "homeassistant/status",
                                             QOS_AT_LEAST_ONCE);
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected.store(false, std::memory_order_relaxed);
            ESP_LOGW(TAG, "disconnected, client retries on its own");
            break;

        case MQTT_EVENT_DATA:
            if (event->current_data_offset == 0) {
                s_inbound.topic.assign(event->topic, event->topic_len);
                s_inbound.payload.clear();
            }
            s_inbound.payload.append(event->data, event->data_len);
            if (s_inbound.payload.size() >= static_cast<std::size_t>(event->total_data_len)) {
                dispatch(s_inbound.topic, s_inbound.payload);
            }
            break;

        default:
            break;
    }
}

}  // namespace

esp_err_t start(const Handlers &handlers)
{
    if (std::string(HASS_MQTT_URI).empty()) {
        ESP_LOGW(TAG, "no broker configured, not starting - see hass_secrets.example.h");
        return ESP_OK;
    }

    s_handlers = handlers;
    s_topics   = protocol::topics_for(HASS_DEVICE_ID);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri                  = HASS_MQTT_URI;
    cfg.credentials.client_id               = HASS_DEVICE_ID "-tab5";
    cfg.credentials.username                = HASS_MQTT_USER;
    cfg.credentials.authentication.password = HASS_MQTT_PASSWORD;

    cfg.session.last_will.topic  = s_topics.availability.c_str();
    cfg.session.last_will.msg    = "offline";
    cfg.session.last_will.qos    = QOS_AT_LEAST_ONCE;
    cfg.session.last_will.retain = RETAIN;
    cfg.session.keepalive        = 30;

    cfg.network.reconnect_timeout_ms = 5000;
    cfg.buffer.size     = 2048;
    cfg.buffer.out_size = 8192;

    s_client = esp_mqtt_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != nullptr, ESP_FAIL, TAG, "init");
    ESP_RETURN_ON_ERROR(
        esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_mqtt_event, nullptr), TAG,
        "register events");
    return esp_mqtt_client_start(s_client);
}

esp_err_t publish(const protocol::Telemetry &telemetry)
{
    if (!s_connected.load(std::memory_order_relaxed)) {
        return ESP_ERR_INVALID_STATE;
    }

    std::string document = protocol::state_document(telemetry);
    const bool forced = s_force_publish.exchange(false, std::memory_order_relaxed);
    if (!forced && document == s_last_state) {
        return ESP_OK;
    }
    s_last_state = document;

    const int result = esp_mqtt_client_enqueue(s_client, s_topics.state.c_str(), document.c_str(),
                                               static_cast<int>(document.size()),
                                               QOS_AT_LEAST_ONCE, RETAIN, true);
    return result >= 0 ? ESP_OK : ESP_FAIL;
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
    s_connected.store(false, std::memory_order_relaxed);
    esp_mqtt_client_stop(s_client);  // not running is fine, that is what is being fixed
    return esp_mqtt_client_start(s_client);
}

}  // namespace hass
