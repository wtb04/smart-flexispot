#include "hass_protocol.h"

#include "deskproto.h"
#include "units.h"

#include "cJSON.h"

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hass::protocol {
namespace {
constexpr std::size_t MAX_TITLE      = 80;
constexpr std::size_t MAX_MESSAGE    = 400;
constexpr int         MIN_TIMEOUT_MS = units::kMsPerSecond;
constexpr int         MAX_TIMEOUT_MS = 2 * units::kMsPerMinute;

constexpr unsigned char UTF8_CONTINUATION_MASK = 0xc0;
constexpr unsigned char UTF8_CONTINUATION      = 0x80;
constexpr unsigned char FIRST_PRINTABLE        = 0x20;

// Rounded, so jitter of a unit or two does not make a new state to publish.
constexpr int   RSSI_STEP_DBM   = 5;
constexpr int   BATTERY_STEP_MA = 10;
constexpr float STEPS_PER_VOLT  = 100.0f;

constexpr int QOS_AT_LEAST_ONCE = 1;

constexpr int MAX_BRIGHTNESS_PERCENT  = 100;
constexpr int BRIGHTNESS_STEP_PERCENT = 5;

constexpr std::size_t PRESET_KEY_SIZE     = 16;
constexpr std::size_t PRESET_PAYLOAD_SIZE = 4;

std::string truncate_utf8(const std::string &text, std::size_t limit)
{
    if (text.size() <= limit) {
        return text;
    }
    std::size_t end = limit;
    while (end > 0 &&
           (static_cast<unsigned char>(text[end]) & UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION) {
        --end;
    }
    return text.substr(0, end);
}

bool has_control_bytes(const std::string &text)
{
    return std::any_of(text.begin(), text.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < FIRST_PRINTABLE && c != '\n' && c != '\t';
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
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

std::string print_and_delete(cJSON *root)
{
    char       *text = cJSON_PrintUnformatted(root);
    std::string out  = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

int truncated_to_step(int value, int step)
{
    return (value / step) * step;
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

cJSON *add_state_entity(cJSON *components, const char *key, const char *platform,
                        const char *name, const std::string &device_id, const Topics &topics,
                        const char *value_template)
{
    add_entity(components, key, platform, name, device_id + "_" + key);
    cJSON *entity = cJSON_GetObjectItem(components, key);
    cJSON_AddStringToObject(entity, "stat_t", topics.state.c_str());
    cJSON_AddStringToObject(entity, "val_tpl", value_template);
    return entity;
}

void add_on_off_payloads(cJSON *entity)
{
    cJSON_AddStringToObject(entity, "pl_on", "ON");
    cJSON_AddStringToObject(entity, "pl_off", "OFF");
}

cJSON *device_block(const std::string &device_id, const std::string &sw_version)
{
    cJSON *device = cJSON_CreateObject();
    cJSON *ids    = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(device_id.c_str()));
    cJSON_AddItemToObject(device, "ids", ids);
    cJSON_AddStringToObject(device, "name", "Smart Flexispot");
    cJSON_AddStringToObject(device, "mf", "Wouter ten Brinke");
    cJSON_AddStringToObject(device, "mdl", "Smart Flexispot");
    cJSON_AddStringToObject(device, "sw", sw_version.c_str());
    cJSON_AddStringToObject(device, "hw", "M5Stack Tab5, ESP32-P4");
    return device;
}

cJSON *origin_block(const std::string &sw_version)
{
    cJSON *origin = cJSON_CreateObject();
    cJSON_AddStringToObject(origin, "name", "smart-flexispot");
    cJSON_AddStringToObject(origin, "sw", sw_version.c_str());
    return origin;
}

void add_desk_entities(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
    cJSON *height = add_state_entity(cmps, "height", "sensor", "Height", device_id, topics,
                                     "{{ value_json.height_cm }}");
    cJSON_AddStringToObject(height, "unit_of_meas", "cm");
    cJSON_AddStringToObject(height, "stat_cla", "measurement");
    cJSON_AddStringToObject(height, "ic", "mdi:arrow-up-down");

    cJSON *desk = add_state_entity(cmps, "desk", "binary_sensor", "Desk link", device_id, topics,
                                   "{{ value_json.desk }}");
    add_on_off_payloads(desk);
    cJSON_AddStringToObject(desk, "dev_cla", "connectivity");
    cJSON_AddStringToObject(desk, "ent_cat", "diagnostic");
}

void add_power_entities(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
    cJSON *battery = add_state_entity(cmps, "battery", "sensor", "Battery", device_id, topics,
                                      "{{ value_json.battery_pct | default('unknown') }}");
    cJSON_AddStringToObject(battery, "unit_of_meas", "%");
    cJSON_AddStringToObject(battery, "dev_cla", "battery");
    cJSON_AddStringToObject(battery, "stat_cla", "measurement");
    cJSON_AddStringToObject(battery, "ent_cat", "diagnostic");

    cJSON *battery_v =
        add_state_entity(cmps, "battery_v", "sensor", "Battery voltage", device_id, topics,
                         "{{ value_json.battery_v | default('unknown') }}");
    cJSON_AddStringToObject(battery_v, "unit_of_meas", "V");
    cJSON_AddStringToObject(battery_v, "dev_cla", "voltage");
    cJSON_AddStringToObject(battery_v, "stat_cla", "measurement");
    cJSON_AddStringToObject(battery_v, "ent_cat", "diagnostic");

    cJSON *charging = add_state_entity(cmps, "charging", "binary_sensor", "Charging", device_id,
                                       topics, "{{ value_json.charging | default('OFF') }}");
    add_on_off_payloads(charging);
    cJSON_AddStringToObject(charging, "dev_cla", "battery_charging");
    cJSON_AddStringToObject(charging, "ent_cat", "diagnostic");

    cJSON *external =
        add_state_entity(cmps, "external_power", "binary_sensor", "External power", device_id,
                         topics, "{{ value_json.external_power | default('ON') }}");
    add_on_off_payloads(external);
    cJSON_AddStringToObject(external, "dev_cla", "plug");
    cJSON_AddStringToObject(external, "ent_cat", "diagnostic");
}

void add_preset_sensor(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
    cJSON *preset = add_state_entity(cmps, "preset", "sensor", "Active preset", device_id, topics,
                                     "{{ value_json.preset | default('none') }}");
    cJSON_AddStringToObject(preset, "dev_cla", "enum");
    cJSON *preset_options = cJSON_AddArrayToObject(preset, "options");
    for (int i = 0; i < deskproto::kPresetCount; ++i) {
        cJSON_AddItemToArray(preset_options, cJSON_CreateString(deskproto::preset_label(i)));
    }
    cJSON_AddItemToArray(preset_options, cJSON_CreateString(deskproto::kBetween));
}

void add_presence_entities(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
    cJSON *presence = add_state_entity(cmps, "presence", "binary_sensor", "Presence", device_id,
                                       topics, "{{ value_json.presence | default('OFF') }}");
    add_on_off_payloads(presence);
    cJSON_AddStringToObject(presence, "dev_cla", "presence");

    cJSON *presence_rssi =
        add_state_entity(cmps, "presence_rssi", "sensor", "Presence signal", device_id, topics,
                         "{{ value_json.presence_rssi | default('unknown') }}");
    cJSON_AddStringToObject(presence_rssi, "unit_of_meas", "dBm");
    cJSON_AddStringToObject(presence_rssi, "dev_cla", "signal_strength");
    cJSON_AddStringToObject(presence_rssi, "stat_cla", "measurement");
    cJSON_AddStringToObject(presence_rssi, "ent_cat", "diagnostic");
}

void add_brightness(cJSON *cmps, const std::string &device_id, const Topics &topics,
                    int brightness_floor)
{
    cJSON *brightness = add_state_entity(cmps, "brightness", "number", "Brightness", device_id,
                                         topics, "{{ value_json.brightness }}");
    cJSON_AddStringToObject(brightness, "cmd_t", topics.cmd_brightness.c_str());
    cJSON_AddNumberToObject(brightness, "min", brightness_floor);
    cJSON_AddNumberToObject(brightness, "max", MAX_BRIGHTNESS_PERCENT);
    cJSON_AddNumberToObject(brightness, "step", BRIGHTNESS_STEP_PERCENT);
    cJSON_AddStringToObject(brightness, "unit_of_meas", "%");
    cJSON_AddStringToObject(brightness, "ent_cat", "config");
    cJSON_AddStringToObject(brightness, "ic", "mdi:brightness-6");
}

void add_system_entities(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
    cJSON *rssi = add_state_entity(cmps, "rssi", "sensor", "Signal", device_id, topics,
                                   "{{ value_json.rssi }}");
    cJSON_AddStringToObject(rssi, "unit_of_meas", "dBm");
    cJSON_AddStringToObject(rssi, "dev_cla", "signal_strength");
    cJSON_AddStringToObject(rssi, "stat_cla", "measurement");
    cJSON_AddStringToObject(rssi, "ent_cat", "diagnostic");

    cJSON *uptime = add_state_entity(cmps, "uptime", "sensor", "Uptime", device_id, topics,
                                     "{{ value_json.uptime_min }}");
    cJSON_AddStringToObject(uptime, "unit_of_meas", "min");
    cJSON_AddStringToObject(uptime, "dev_cla", "duration");
    cJSON_AddStringToObject(uptime, "ent_cat", "diagnostic");

    cJSON *motion = add_state_entity(cmps, "motion", "sensor", "Motion", device_id, topics,
                                     "{{ value_json.motion }}");
    cJSON_AddStringToObject(motion, "ic", "mdi:desk");
}

void add_move_buttons(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
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
}

void add_preset_buttons(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
    for (int preset = 1; preset <= deskproto::kPresetCount; ++preset) {
        char key[PRESET_KEY_SIZE];
        char payload[PRESET_PAYLOAD_SIZE];
        std::snprintf(key, sizeof(key), "preset%d", preset);
        std::snprintf(payload, sizeof(payload), "%d", preset);

        add_entity(cmps, key, "button", deskproto::preset_label(preset - 1),
                   device_id + "_" + key);
        cJSON *button = cJSON_GetObjectItem(cmps, key);
        cJSON_AddStringToObject(button, "cmd_t", topics.cmd_preset.c_str());
        cJSON_AddStringToObject(button, "pl_prs", payload);
        cJSON_AddStringToObject(button, "ic", "mdi:desk");
    }
}

void add_screen_entities(cJSON *cmps, const std::string &device_id, const Topics &topics)
{
    cJSON *screen = add_state_entity(cmps, "screen", "switch", "Screen", device_id, topics,
                                     "{{ value_json.screen | default('ON') }}");
    cJSON_AddStringToObject(screen, "cmd_t", topics.cmd_screen.c_str());
    add_on_off_payloads(screen);
    cJSON_AddStringToObject(screen, "ic", "mdi:monitor");

    add_entity(cmps, "notify", "notify", "Screen message", device_id + "_notify");
    cJSON *notify = cJSON_GetObjectItem(cmps, "notify");
    cJSON_AddStringToObject(notify, "cmd_t", topics.cmd_notify.c_str());
    cJSON_AddStringToObject(notify, "ic", "mdi:message-text");
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
        device_id + "/cmd/screen",
    };
}

std::string state_document(const Telemetry &t)
{
    cJSON *root = cJSON_CreateObject();

    if (t.height_mm >= 0) {
        cJSON_AddNumberToObject(root, "height_mm", t.height_mm);
        const double cm = static_cast<double>(t.height_mm) / units::kMmPerCm;
        cJSON_AddNumberToObject(root, "height_cm", cm);
    }
    cJSON_AddStringToObject(root, "desk", t.desk_connected ? "ON" : "OFF");
    cJSON_AddStringToObject(root, "motion", t.motion);

    if (t.battery_percent >= 0) {
        cJSON_AddNumberToObject(root, "battery_pct", t.battery_percent);
        cJSON_AddNumberToObject(root, "battery_v",
                                static_cast<int>(t.battery_volts * STEPS_PER_VOLT + 0.5f) /
                                    static_cast<double>(STEPS_PER_VOLT));
        cJSON_AddNumberToObject(root, "battery_ma",
                                truncated_to_step(t.battery_milliamps, BATTERY_STEP_MA));
        cJSON_AddStringToObject(root, "battery_present", "ON");
        cJSON_AddStringToObject(root, "charging", t.charging ? "ON" : "OFF");
        cJSON_AddStringToObject(root, "external_power", t.on_battery ? "OFF" : "ON");
    } else {
        cJSON_AddStringToObject(root, "battery_present", "OFF");
    }

    cJSON_AddStringToObject(root, "preset", t.preset);
    cJSON_AddStringToObject(root, "presence", t.presence ? "ON" : "OFF");
    cJSON_AddStringToObject(root, "screen", t.screen ? "ON" : "OFF");
    cJSON_AddNumberToObject(root, "presence_rssi",
                            truncated_to_step(t.presence_rssi, RSSI_STEP_DBM));

    cJSON_AddNumberToObject(root, "brightness", t.brightness);
    cJSON_AddNumberToObject(root, "rssi", truncated_to_step(t.rssi_dbm, RSSI_STEP_DBM));
    cJSON_AddNumberToObject(root, "uptime_min", t.uptime_s / units::kSecondsPerMinute);
    cJSON_AddNumberToObject(root, "free_heap_kb", t.free_heap / units::kBytesPerKiB);
    if (!t.ip_address.empty()) {
        cJSON_AddStringToObject(root, "ip", t.ip_address.c_str());
    }

    return print_and_delete(root);
}

std::string discovery_document(const std::string &device_id, const std::string &sw_version,
                               int brightness_floor)
{
    const Topics topics = topics_for(device_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "dev", device_block(device_id, sw_version));
    cJSON_AddItemToObject(root, "o", origin_block(sw_version));

    cJSON_AddStringToObject(root, "avty_t", topics.availability.c_str());
    cJSON_AddStringToObject(root, "pl_avail", "online");
    cJSON_AddStringToObject(root, "pl_not_avail", "offline");
    cJSON_AddNumberToObject(root, "qos", QOS_AT_LEAST_ONCE);

    cJSON *cmps = cJSON_CreateObject();
    add_desk_entities(cmps, device_id, topics);
    add_power_entities(cmps, device_id, topics);
    add_preset_sensor(cmps, device_id, topics);
    add_presence_entities(cmps, device_id, topics);
    add_brightness(cmps, device_id, topics, brightness_floor);
    add_system_entities(cmps, device_id, topics);
    add_move_buttons(cmps, device_id, topics);
    add_preset_buttons(cmps, device_id, topics);
    add_screen_entities(cmps, device_id, topics);
    cJSON_AddItemToObject(root, "cmps", cmps);

    return print_and_delete(root);
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
        out.timeout_ms = static_cast<int>(timeout_s->valuedouble * units::kMsPerSecond);
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

bool parse_screen(const std::string &payload, bool &on)
{
    std::string text;
    for (char c : payload) {
        text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (text.find("on") != std::string::npos || text == "1" || text == "true") {
        on = true;
        return true;
    }
    if (text.find("off") != std::string::npos || text == "0" || text == "false") {
        on = false;
        return true;
    }
    return false;
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
    if (payload.size() == 1 && payload[0] >= '1' && payload[0] < '1' + deskproto::kPresetCount) {
        return payload[0] - '0';
    }
    return 0;
}

int parse_brightness(const std::string &payload)
{
    char       *end   = nullptr;
    const long  value = std::strtol(payload.c_str(), &end, 10);
    if (end == payload.c_str() || value < 0 || value > MAX_BRIGHTNESS_PERCENT) {
        return -1;
    }
    return static_cast<int>(value);
}

}  // namespace hass::protocol
