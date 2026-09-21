#include "hass_protocol.h"

#include "cJSON.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hass::protocol {
namespace {

constexpr std::size_t MAX_TITLE   = 80;
constexpr std::size_t MAX_MESSAGE = 400;
constexpr int MIN_TIMEOUT_MS = 1000;
constexpr int MAX_TIMEOUT_MS = 120000;

std::string truncate_utf8(const std::string &text, std::size_t limit)
{
    if (text.size() <= limit) {
        return text;
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

bool has_control_bytes(const std::string &text)
{
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 && c != '\n' && c != '\t';
    });
}

bool looks_like_json(const std::string &text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    return first != std::string::npos && (text[first] == '{' || text[first] == '[');
}

std::string normalise_level(const std::string &level)
{
    if (level == "error" || level == "alert") return "error";
    if (level == "warning" || level == "warn") return "warning";
    if (level == "success" || level == "ok") return "success";
    return "info";  // unknown levels degrade rather than drop the message
}

std::string string_field(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    // An automation sending {"message": 42} must not take the notification down.
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

void add_entity(cJSON *components, const char *key, const char *platform, const char *name,
                const std::string &unique_id)
{
    cJSON *entity = cJSON_CreateObject();
    cJSON_AddStringToObject(entity, "p", platform);
    if (name != nullptr) {
        cJSON_AddStringToObject(entity, "name", name);
    } else {
        cJSON_AddNullToObject(entity, "name");
    }
    cJSON_AddStringToObject(entity, "uniq_id", unique_id.c_str());
    cJSON_AddItemToObject(components, key, entity);
}

}  // namespace

Topics topics_for(const std::string &device_id)
{
    return Topics{
        device_id + "/availability",
        device_id + "/state",
        "homeassistant/device/" + device_id + "/config",
        device_id + "/cmd/#",
        device_id + "/cmd/preset",
        device_id + "/cmd/brightness",
        device_id + "/cmd/notify",
        device_id + "/cmd/move",
    };
}

std::string state_document(const Telemetry &t)
{
    cJSON *root = cJSON_CreateObject();

    if (t.height_mm >= 0) {
        cJSON_AddNumberToObject(root, "height_mm", t.height_mm);
        // In double: rounding in float and letting serialisation promote turns
        // 72.8 into 72.80000305175781.
        const double cm = static_cast<double>(t.height_mm) / 10.0;
        cJSON_AddNumberToObject(root, "height_cm", cm);
    }
    cJSON_AddStringToObject(root, "desk", t.desk_connected ? "ON" : "OFF");
    cJSON_AddStringToObject(root, "motion", t.motion);

    if (t.battery_percent >= 0) {
        cJSON_AddNumberToObject(root, "battery_pct", t.battery_percent);
        cJSON_AddNumberToObject(root, "battery_v",
                                static_cast<int>(t.battery_volts * 100.0f + 0.5f) / 100.0);
        // Quantised: the raw reading jitters by a milliamp, which would republish
        // on every tick.
        cJSON_AddNumberToObject(root, "battery_ma", (t.battery_milliamps / 10) * 10);
        cJSON_AddStringToObject(root, "battery_present", "ON");
        cJSON_AddStringToObject(root, "charging", t.charging ? "ON" : "OFF");
        cJSON_AddStringToObject(root, "external_power", t.on_battery ? "OFF" : "ON");
    } else {
        cJSON_AddStringToObject(root, "battery_present", "OFF");
    }

    cJSON_AddStringToObject(root, "preset", t.preset);
    cJSON_AddStringToObject(root, "presence", t.presence ? "ON" : "OFF");
    // Quantised; -127 stands in for "nothing heard".
    cJSON_AddNumberToObject(root, "presence_rssi", (t.presence_rssi / 5) * 5);

    cJSON_AddNumberToObject(root, "brightness", t.brightness);
    cJSON_AddNumberToObject(root, "rssi", (t.rssi_dbm / 5) * 5);
    // Minutes and KiB: a value ticking every second would defeat publish-on-change.
    cJSON_AddNumberToObject(root, "uptime_min", t.uptime_s / 60);
    cJSON_AddNumberToObject(root, "free_heap_kb", t.free_heap / 1024);
    if (!t.ip_address.empty()) {
        cJSON_AddStringToObject(root, "ip", t.ip_address.c_str());
    }

    char *text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string discovery_document(const std::string &device_id, const std::string &sw_version)
{
    const Topics topics = topics_for(device_id);

    cJSON *root = cJSON_CreateObject();

    cJSON *device = cJSON_CreateObject();
    cJSON *ids    = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(device_id.c_str()));
    cJSON_AddItemToObject(device, "ids", ids);
    cJSON_AddStringToObject(device, "name", "Smart Flexispot");
    cJSON_AddStringToObject(device, "mf", "Wouter ten Brinke");
    cJSON_AddStringToObject(device, "mdl", "Smart Flexispot (M5Stack Tab5)");
    cJSON_AddStringToObject(device, "sw", sw_version.c_str());
    cJSON_AddStringToObject(device, "hw", "Tab5 / ESP32-P4");
    cJSON_AddItemToObject(root, "dev", device);

    // Required for device-based discovery; the whole payload is rejected without it.
    cJSON *origin = cJSON_CreateObject();
    cJSON_AddStringToObject(origin, "name", "tab5-hello");
    cJSON_AddStringToObject(origin, "sw", sw_version.c_str());
    cJSON_AddItemToObject(root, "o", origin);

    cJSON_AddStringToObject(root, "avty_t", topics.availability.c_str());
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    cJSON_AddNumberToObject(root, "qos", 1);

    cJSON *cmps = cJSON_CreateObject();

    add_entity(cmps, "height", "sensor", "Height", device_id + "_height");
    cJSON *height = cJSON_GetObjectItem(cmps, "height");
    cJSON_AddStringToObject(height, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(height, "val_tpl", "{{ value_json.height_cm }}");
    cJSON_AddStringToObject(height, "unit_of_meas", "cm");
    cJSON_AddStringToObject(height, "stat_cla", "measurement");
    cJSON_AddStringToObject(height, "ic", "mdi:arrow-up-down");

    add_entity(cmps, "desk", "binary_sensor", "Desk link", device_id + "_desk");
    cJSON *desk = cJSON_GetObjectItem(cmps, "desk");
    cJSON_AddStringToObject(desk, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(desk, "val_tpl", "{{ value_json.desk }}");
    cJSON_AddStringToObject(desk, "pl_on", "ON");
    cJSON_AddStringToObject(desk, "pl_off", "OFF");
    cJSON_AddStringToObject(desk, "dev_cla", "connectivity");
    cJSON_AddStringToObject(desk, "ent_cat", "diagnostic");

    add_entity(cmps, "battery", "sensor", "Battery", device_id + "_battery");
    cJSON *battery = cJSON_GetObjectItem(cmps, "battery");
    cJSON_AddStringToObject(battery, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(battery, "val_tpl", "{{ value_json.battery_pct | default('unknown') }}");
    cJSON_AddStringToObject(battery, "unit_of_meas", "%");
    cJSON_AddStringToObject(battery, "dev_cla", "battery");
    cJSON_AddStringToObject(battery, "stat_cla", "measurement");
    cJSON_AddStringToObject(battery, "ent_cat", "diagnostic");

    add_entity(cmps, "battery_v", "sensor", "Battery voltage", device_id + "_battery_v");
    cJSON *battery_v = cJSON_GetObjectItem(cmps, "battery_v");
    cJSON_AddStringToObject(battery_v, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(battery_v, "val_tpl", "{{ value_json.battery_v | default('unknown') }}");
    cJSON_AddStringToObject(battery_v, "unit_of_meas", "V");
    cJSON_AddStringToObject(battery_v, "dev_cla", "voltage");
    cJSON_AddStringToObject(battery_v, "stat_cla", "measurement");
    cJSON_AddStringToObject(battery_v, "ent_cat", "diagnostic");

    add_entity(cmps, "charging", "binary_sensor", "Charging", device_id + "_charging");
    cJSON *charging = cJSON_GetObjectItem(cmps, "charging");
    cJSON_AddStringToObject(charging, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(charging, "val_tpl", "{{ value_json.charging | default('OFF') }}");
    cJSON_AddStringToObject(charging, "pl_on", "ON");
    cJSON_AddStringToObject(charging, "pl_off", "OFF");
    cJSON_AddStringToObject(charging, "dev_cla", "battery_charging");
    cJSON_AddStringToObject(charging, "ent_cat", "diagnostic");

    add_entity(cmps, "external_power", "binary_sensor", "External power",
               device_id + "_external_power");
    cJSON *external = cJSON_GetObjectItem(cmps, "external_power");
    cJSON_AddStringToObject(external, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(external, "val_tpl",
                            "{{ value_json.external_power | default('ON') }}");
    cJSON_AddStringToObject(external, "pl_on", "ON");
    cJSON_AddStringToObject(external, "pl_off", "OFF");
    cJSON_AddStringToObject(external, "dev_cla", "plug");
    cJSON_AddStringToObject(external, "ent_cat", "diagnostic");

    add_entity(cmps, "preset", "sensor", "Active preset", device_id + "_preset");
    cJSON *preset = cJSON_GetObjectItem(cmps, "preset");
    cJSON_AddStringToObject(preset, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(preset, "val_tpl", "{{ value_json.preset | default('none') }}");
    cJSON_AddStringToObject(preset, "dev_cla", "enum");
    cJSON *preset_options = cJSON_AddArrayToObject(preset, "options");
    for (const char *option : {"stand", "sit", "preset_1", "preset_2", "none"}) {
        cJSON_AddItemToArray(preset_options, cJSON_CreateString(option));
    }

    add_entity(cmps, "presence", "binary_sensor", "Presence", device_id + "_presence");
    cJSON *presence = cJSON_GetObjectItem(cmps, "presence");
    cJSON_AddStringToObject(presence, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(presence, "val_tpl", "{{ value_json.presence | default('OFF') }}");
    cJSON_AddStringToObject(presence, "pl_on", "ON");
    cJSON_AddStringToObject(presence, "pl_off", "OFF");
    cJSON_AddStringToObject(presence, "dev_cla", "presence");

    add_entity(cmps, "presence_rssi", "sensor", "Presence signal",
               device_id + "_presence_rssi");
    cJSON *presence_rssi = cJSON_GetObjectItem(cmps, "presence_rssi");
    cJSON_AddStringToObject(presence_rssi, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(presence_rssi, "val_tpl",
                            "{{ value_json.presence_rssi | default('unknown') }}");
    cJSON_AddStringToObject(presence_rssi, "unit_of_meas", "dBm");
    cJSON_AddStringToObject(presence_rssi, "dev_cla", "signal_strength");
    cJSON_AddStringToObject(presence_rssi, "stat_cla", "measurement");
    cJSON_AddStringToObject(presence_rssi, "ent_cat", "diagnostic");

    add_entity(cmps, "brightness", "number", "Brightness", device_id + "_brightness");
    cJSON *brightness = cJSON_GetObjectItem(cmps, "brightness");
    cJSON_AddStringToObject(brightness, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(brightness, "val_tpl", "{{ value_json.brightness }}");
    cJSON_AddStringToObject(brightness, "cmd_t", topics.cmd_brightness.c_str());
    cJSON_AddNumberToObject(brightness, "min", 20);  // panel floor
    cJSON_AddNumberToObject(brightness, "max", 100);
    cJSON_AddNumberToObject(brightness, "step", 5);
    cJSON_AddStringToObject(brightness, "unit_of_meas", "%");
    cJSON_AddStringToObject(brightness, "ent_cat", "config");
    cJSON_AddStringToObject(brightness, "ic", "mdi:brightness-6");

    add_entity(cmps, "rssi", "sensor", "Signal", device_id + "_rssi");
    cJSON *rssi = cJSON_GetObjectItem(cmps, "rssi");
    cJSON_AddStringToObject(rssi, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(rssi, "val_tpl", "{{ value_json.rssi }}");
    cJSON_AddStringToObject(rssi, "unit_of_meas", "dBm");
    cJSON_AddStringToObject(rssi, "dev_cla", "signal_strength");
    cJSON_AddStringToObject(rssi, "stat_cla", "measurement");
    cJSON_AddStringToObject(rssi, "ent_cat", "diagnostic");

    add_entity(cmps, "uptime", "sensor", "Uptime", device_id + "_uptime");
    cJSON *uptime = cJSON_GetObjectItem(cmps, "uptime");
    cJSON_AddStringToObject(uptime, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(uptime, "val_tpl", "{{ value_json.uptime_min }}");
    cJSON_AddStringToObject(uptime, "unit_of_meas", "min");
    cJSON_AddStringToObject(uptime, "dev_cla", "duration");
    cJSON_AddStringToObject(uptime, "ent_cat", "diagnostic");

    add_entity(cmps, "motion", "sensor", "Motion", device_id + "_motion");
    cJSON *motion = cJSON_GetObjectItem(cmps, "motion");
    cJSON_AddStringToObject(motion, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(motion, "val_tpl", "{{ value_json.motion }}");
    cJSON_AddStringToObject(motion, "ic", "mdi:desk");

    struct MoveButton {
        const char *key;
        const char *name;
        const char *payload;
        const char *icon;
    };
    constexpr MoveButton kMoveButtons[] = {
        {"move_up", "Up", "UP", "mdi:arrow-up-bold"},
        {"move_down", "Down", "DOWN", "mdi:arrow-down-bold"},
        {"move_stop", "Stop", "STOP", "mdi:stop"},
    };
    for (const MoveButton &button : kMoveButtons) {
        add_entity(cmps, button.key, "button", button.name, device_id + "_" + button.key);
        cJSON *entity = cJSON_GetObjectItem(cmps, button.key);
        cJSON_AddStringToObject(entity, "cmd_t", topics.cmd_move.c_str());
        cJSON_AddStringToObject(entity, "pl_prs", button.payload);
        cJSON_AddStringToObject(entity, "ic", button.icon);
    }

    for (int preset = 1; preset <= 4; ++preset) {
        char key[16];
        char name[16];
        char payload[4];
        std::snprintf(key, sizeof(key), "preset%d", preset);
        std::snprintf(name, sizeof(name), "Preset %d", preset);
        std::snprintf(payload, sizeof(payload), "%d", preset);

        add_entity(cmps, key, "button", name, device_id + "_" + key);
        cJSON *button = cJSON_GetObjectItem(cmps, key);
        cJSON_AddStringToObject(button, "cmd_t", topics.cmd_preset.c_str());
        cJSON_AddStringToObject(button, "pl_prs", payload);
        cJSON_AddStringToObject(button, "ic", "mdi:desk");
    }

    add_entity(cmps, "notify", "notify", "Screen message", device_id + "_notify");
    cJSON *notify = cJSON_GetObjectItem(cmps, "notify");
    cJSON_AddStringToObject(notify, "cmd_t", topics.cmd_notify.c_str());
    cJSON_AddStringToObject(notify, "ic", "mdi:message-text");

    cJSON_AddItemToObject(root, "cmps", cmps);

    char *text = cJSON_PrintUnformatted(root);
    std::string out = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

Notification parse_notification(const std::string &payload)
{
    Notification out;
    if (payload.empty() || has_control_bytes(payload)) {
        return out;
    }

    if (!looks_like_json(payload)) {
        out.valid   = true;
        out.message = truncate_utf8(payload, MAX_MESSAGE);
        out.level   = "info";
        return out;
    }

    cJSON *root = cJSON_Parse(payload.c_str());
    if (root == nullptr || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return out;
    }

    out.title   = truncate_utf8(string_field(root, "title"), MAX_TITLE);
    out.message = truncate_utf8(string_field(root, "message"), MAX_MESSAGE);
    out.level   = normalise_level(string_field(root, "level"));

    const cJSON *timeout_s  = cJSON_GetObjectItemCaseSensitive(root, "timeout_s");
    const cJSON *timeout_ms = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
    if (cJSON_IsNumber(timeout_s)) {
        out.timeout_ms = static_cast<int>(timeout_s->valuedouble * 1000);
    } else if (cJSON_IsNumber(timeout_ms)) {
        out.timeout_ms = static_cast<int>(timeout_ms->valuedouble);
    }
    if (out.timeout_ms != 0) {
        out.timeout_ms = std::clamp(out.timeout_ms, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS);
    }

    out.valid = !out.message.empty() || !out.title.empty();
    cJSON_Delete(root);
    return out;
}

Move parse_move(const std::string &payload)
{
    if (payload == "UP") return Move::Up;
    if (payload == "DOWN") return Move::Down;
    if (payload == "STOP") return Move::Stop;
    return Move::Unknown;
}

int parse_preset(const std::string &payload)
{
    if (payload.size() == 1 && payload[0] >= '1' && payload[0] <= '4') {
        return payload[0] - '0';
    }
    return 0;
}

int parse_brightness(const std::string &payload)
{
    char       *end   = nullptr;
    const long  value = std::strtol(payload.c_str(), &end, 10);
    if (end == payload.c_str() || value < 0 || value > 100) {
        return -1;
    }
    return static_cast<int>(value);
}

}  // namespace hass::protocol
