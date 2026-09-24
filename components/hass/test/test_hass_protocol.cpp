#include "hass_protocol.h"

#include "cJSON.h"

#include <cstdio>
#include <string>

namespace {
using namespace hass::protocol;

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

/** Pulls a value out of a JSON document so tests read against shape, not text. */
std::string field(const std::string &json, const char *key)
{
    cJSON *root = cJSON_Parse(json.c_str());
    if (root == nullptr) {
        return "<unparseable>";
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    std::string  out  = "<missing>";
    if (cJSON_IsString(item)) {
        out = item->valuestring;
    } else if (cJSON_IsNumber(item)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", item->valuedouble);
        out = buf;
    }
    cJSON_Delete(root);
    return out;
}

void test_topics()
{
    const Topics t = topics_for("tab5_desk");
    check(t.availability == "tab5_desk/availability", "availability topic");
    check(t.state == "tab5_desk/state", "state topic");
    check(t.discovery == "homeassistant/device/tab5_desk/config", "device-based discovery topic");
    check(t.command == "tab5_desk/cmd/#", "command wildcard");
}

void test_state_document()
{
    Telemetry t;
    t.height_mm         = 728;
    t.desk_connected    = true;
    t.battery_percent   = 84;
    t.battery_volts     = 8.09f;
    t.brightness        = 80;

    const std::string doc = state_document(t);
    check(field(doc, "height_cm") == "72.8", "height in cm without float noise");
    check(field(doc, "height_mm") == "728", "height in mm");
    check(field(doc, "desk") == "ON", "desk link state");
    check(field(doc, "battery_pct") == "84", "battery percent");
    check(field(doc, "battery_present") == "ON", "battery present");

    Telemetry empty;
    empty.battery_percent = -1;
    const std::string no_batt = state_document(empty);
    check(field(no_batt, "battery_present") == "OFF", "absent battery reported as absent");
    check(field(no_batt, "battery_pct") == "<missing>", "no percentage invented when absent");

    check(field(no_batt, "height_mm") == "<missing>", "unknown height omitted");
}

void test_noise_quantisation()
{
    Telemetry a;
    a.battery_percent = 84;
    a.battery_volts   = 8.09f;
    a.rssi_dbm        = -57;
    a.battery_milliamps = 0;

    Telemetry b = a;
    b.rssi_dbm          = -56;
    b.battery_milliamps = 1;

    check(state_document(a) == state_document(b), "1 dBm and 1 mA jitter produce no change");

    Telemetry c = a;
    c.rssi_dbm = -70;
    check(state_document(a) != state_document(c), "a real signal change still shows");
}

void test_charge_state()
{
    Telemetry running;
    running.battery_percent = 82;
    running.battery_volts   = 7.9f;
    running.on_battery      = true;
    running.charging        = false;
    const std::string on_batt = state_document(running);
    check(field(on_batt, "charging") == "OFF", "discharging is not charging");
    check(field(on_batt, "external_power") == "OFF", "on battery means no external power");

    Telemetry charging;
    charging.battery_percent = 82;
    charging.battery_volts   = 8.1f;
    charging.charging        = true;
    const std::string chg = state_document(charging);
    check(field(chg, "charging") == "ON", "charging reported");
    check(field(chg, "external_power") == "ON", "charging implies external power");

    Telemetry absent;
    absent.battery_percent = -1;
    check(field(state_document(absent), "charging") == "<missing>",
          "no charge claim without a battery");
}

void test_discovery_document()
{
    const std::string doc = discovery_document("tab5_desk", "1.0.0");
    cJSON            *root = cJSON_Parse(doc.c_str());
    check(root != nullptr, "discovery payload parses");
    if (root == nullptr) {
        return;
    }

    check(cJSON_GetObjectItem(root, "dev") != nullptr, "device block present");
    check(cJSON_GetObjectItem(root, "o") != nullptr, "origin block present");
    check(cJSON_GetObjectItem(root, "avty_t") != nullptr, "availability topic present");

    const cJSON *cmps = cJSON_GetObjectItem(root, "cmps");
    check(cmps != nullptr, "components block present");

    check(doc.find("\"object_id\"") == std::string::npos, "no removed object_id key");

    bool all_unique = true;
    int  count      = 0;
    for (const cJSON *entity = cmps != nullptr ? cmps->child : nullptr; entity != nullptr;
         entity              = entity->next) {
        ++count;
        all_unique &= cJSON_IsString(cJSON_GetObjectItem(entity, "uniq_id"));
        all_unique &= cJSON_IsString(cJSON_GetObjectItem(entity, "p"));
    }
    check(count >= 10, "all entities present");
    check(all_unique, "every entity has a unique_id and platform");

    cJSON_Delete(root);
}

void test_notification_parsing()
{
    const Notification plain = parse_notification("Wasmachine is klaar");
    check(plain.valid && plain.message == "Wasmachine is klaar" && plain.level == "info",
          "plain text notification");

    const Notification rich = parse_notification(
        R"({"title":"Warning","message":"Window open","level":"warn","timeout_s":12})");
    check(rich.valid && rich.title == "Warning" && rich.message == "Window open", "json fields");
    check(rich.level == "warning", "level alias normalised");
    check(rich.timeout_ms == 12000, "timeout_s converted to ms");

    check(!parse_notification("{not json at all").valid, "broken json rejected");
    const Notification numeric = parse_notification(R"({"message":42,"title":"Hi"})");
    check(numeric.valid && numeric.message.empty() && numeric.title == "Hi",
          "non-string field ignored rather than fatal");
    check(!parse_notification(std::string("bad\x01payload")).valid, "control bytes rejected");
    check(!parse_notification(R"({"level":"info"})").valid, "no text means nothing to show");
    check(parse_notification(R"({"title":"Only a title"})").valid, "title alone is enough");

    check(parse_notification(R"({"message":"x","timeout_s":9999})").timeout_ms == 120000,
          "timeout clamped");

    const std::string long_utf8 = std::string(399, 'a') + "\xc3\xa9";
    const Notification truncated = parse_notification(long_utf8);
    check(truncated.valid && truncated.message.size() == 399, "utf-8 not split by truncation");
}

void test_command_parsing()
{
    check(parse_preset("1") == 1 && parse_preset("6") == 6, "preset payloads");
    check(parse_preset("0") == 0 && parse_preset("7") == 0 && parse_preset("") == 0,
          "out-of-range presets rejected");
    check(parse_brightness("50") == 50, "brightness payload");
    check(parse_brightness("101") == -1 && parse_brightness("abc") == -1,
          "invalid brightness rejected");
}

}  // namespace

int main()
{
    test_topics();
    test_state_document();
    test_noise_quantisation();
    test_charge_state();
    test_discovery_document();
    test_notification_parsing();
    test_command_parsing();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
