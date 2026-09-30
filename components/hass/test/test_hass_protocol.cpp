#include "hass_protocol.h"

#include "cJSON.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

namespace {
using namespace hass::protocol;

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

TEST(HassProtocol, topics)
{
    const Topics t = topics_for("tab5_desk");
    EXPECT_EQ(t.availability, "tab5_desk/availability") << "availability topic";
    EXPECT_EQ(t.state, "tab5_desk/state") << "state topic";
    EXPECT_EQ(t.discovery, "homeassistant/device/tab5_desk/config") << "device-based discovery topic";
    EXPECT_EQ(t.command, "tab5_desk/cmd/#") << "command wildcard";
}

TEST(HassProtocol, state_document)
{
    Telemetry t;
    t.height_mm         = 728;
    t.desk_connected    = true;
    t.battery_percent   = 84;
    t.battery_volts     = 8.09f;
    t.brightness        = 80;

    const std::string doc = state_document(t);
    EXPECT_EQ(field(doc, "height_cm"), "72.8") << "height in cm without float noise";
    EXPECT_EQ(field(doc, "height_mm"), "728") << "height in mm";
    EXPECT_EQ(field(doc, "desk"), "ON") << "desk link state";
    EXPECT_EQ(field(doc, "battery_pct"), "84") << "battery percent";
    EXPECT_EQ(field(doc, "battery_present"), "ON") << "battery present";

    Telemetry empty;
    empty.battery_percent = -1;
    const std::string no_batt = state_document(empty);
    EXPECT_EQ(field(no_batt, "battery_present"), "OFF") << "absent battery reported as absent";
    EXPECT_EQ(field(no_batt, "battery_pct"), "<missing>") << "no percentage invented when absent";

    EXPECT_EQ(field(no_batt, "height_mm"), "<missing>") << "unknown height omitted";
}

TEST(HassProtocol, noise_quantisation)
{
    Telemetry a;
    a.battery_percent = 84;
    a.battery_volts   = 8.09f;
    a.rssi_dbm        = -57;
    a.battery_milliamps = 0;

    Telemetry b = a;
    b.rssi_dbm          = -56;
    b.battery_milliamps = 1;

    EXPECT_EQ(state_document(a), state_document(b)) << "1 dBm and 1 mA jitter produce no change";

    Telemetry c = a;
    c.rssi_dbm = -70;
    EXPECT_NE(state_document(a), state_document(c)) << "a real signal change still shows";
}

TEST(HassProtocol, charge_state)
{
    Telemetry running;
    running.battery_percent = 82;
    running.battery_volts   = 7.9f;
    running.on_battery      = true;
    running.charging        = false;
    const std::string on_batt = state_document(running);
    EXPECT_EQ(field(on_batt, "charging"), "OFF") << "discharging is not charging";
    EXPECT_EQ(field(on_batt, "external_power"), "OFF") << "on battery means no external power";

    Telemetry charging;
    charging.battery_percent = 82;
    charging.battery_volts   = 8.1f;
    charging.charging        = true;
    const std::string chg = state_document(charging);
    EXPECT_EQ(field(chg, "charging"), "ON") << "charging reported";
    EXPECT_EQ(field(chg, "external_power"), "ON") << "charging implies external power";

    Telemetry absent;
    absent.battery_percent = -1;
    EXPECT_EQ(field(state_document(absent), "charging"), "<missing>") << "no charge claim without a battery";
}

TEST(HassProtocol, discovery_document)
{
    const std::string doc = discovery_document("tab5_desk", "1.0.0", 20);
    cJSON            *root = cJSON_Parse(doc.c_str());
    EXPECT_NE(root, nullptr) << "discovery payload parses";
    if (root == nullptr) {
        return;
    }

    EXPECT_NE(cJSON_GetObjectItem(root, "dev"), nullptr) << "device block present";
    EXPECT_NE(cJSON_GetObjectItem(root, "o"), nullptr) << "origin block present";
    EXPECT_NE(cJSON_GetObjectItem(root, "avty_t"), nullptr) << "availability topic present";

    const cJSON *cmps = cJSON_GetObjectItem(root, "cmps");
    EXPECT_NE(cmps, nullptr) << "components block present";

    EXPECT_EQ(doc.find("\"object_id\""), std::string::npos) << "no removed object_id key";

    bool all_unique = true;
    int  count      = 0;
    for (const cJSON *entity = cmps != nullptr ? cmps->child : nullptr; entity != nullptr;
         entity              = entity->next) {
        ++count;
        all_unique &= cJSON_IsString(cJSON_GetObjectItem(entity, "uniq_id"));
        all_unique &= cJSON_IsString(cJSON_GetObjectItem(entity, "p"));
    }
    EXPECT_TRUE(count >= 10) << "all entities present";
    EXPECT_TRUE(all_unique) << "every entity has a unique_id and platform";

    cJSON_Delete(root);
}

TEST(HassProtocol, last_update)
{
    Telemetry t;
    EXPECT_EQ(field(state_document(t), "last_update"), "installed") << "an update that took, or none, says so";
    t.last_update = "rolled back: 1.4.0";
    EXPECT_EQ(field(state_document(t), "last_update"), "rolled back: 1.4.0") << "one turned back from says which";

    const std::string doc  = discovery_document("tab5_desk", "1.0.0", 20);
    cJSON            *root = cJSON_Parse(doc.c_str());
    ASSERT_NE(root, nullptr);
    const cJSON *entity = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "cmps"), "last_update");
    ASSERT_NE(entity, nullptr) << "Home Assistant is told of the sensor";
    EXPECT_STREQ(cJSON_GetObjectItem(entity, "val_tpl")->valuestring, "{{ value_json.last_update }}");
    EXPECT_STREQ(cJSON_GetObjectItem(entity, "ent_cat")->valuestring, "diagnostic");
    cJSON_Delete(root);
}

TEST(HassProtocol, notification_parsing)
{
    const Notification plain = parse_notification("Wasmachine is klaar");
    EXPECT_TRUE(plain.valid && plain.message == "Wasmachine is klaar" && plain.level == "info") << "plain text notification";

    const Notification rich = parse_notification(
        R"({"title":"Warning","message":"Window open","level":"warn","timeout_s":12})");
    EXPECT_TRUE(rich.valid && rich.title == "Warning" && rich.message == "Window open") << "json fields";
    EXPECT_EQ(rich.level, "warning") << "level alias normalised";
    EXPECT_EQ(rich.timeout_ms, 12000) << "timeout_s converted to ms";

    EXPECT_TRUE(!parse_notification("{not json at all").valid) << "broken json rejected";
    const Notification numeric = parse_notification(R"({"message":42,"title":"Hi"})");
    EXPECT_TRUE(numeric.valid && numeric.message.empty() && numeric.title == "Hi") << "non-string field ignored rather than fatal";
    EXPECT_TRUE(!parse_notification(std::string("bad\x01payload")).valid) << "control bytes rejected";
    EXPECT_TRUE(!parse_notification(R"({"level":"info"})").valid) << "no text means nothing to show";
    EXPECT_TRUE(parse_notification(R"({"title":"Only a title"})").valid) << "title alone is enough";

    EXPECT_EQ(parse_notification(R"({"message":"x","timeout_s":9999})").timeout_ms, 600000) << "timeout clamped to ten minutes";
    EXPECT_TRUE(parse_notification(R"({"message":"x","timeout_s":0})").timeout_ms < 0) << "a timeout of 0 keeps it until tapped";
    EXPECT_EQ(parse_notification(R"({"message":"x"})").timeout_ms, 0) << "no timeout, the default";

    const std::string long_utf8 = std::string(399, 'a') + "\xc3\xa9";
    const Notification truncated = parse_notification(long_utf8);
    EXPECT_TRUE(truncated.valid && truncated.message.size() == 399) << "utf-8 not split by truncation";
}

TEST(HassProtocol, command_parsing)
{
    EXPECT_TRUE(parse_preset("1") == 1 && parse_preset("6") == 6) << "preset payloads";
    EXPECT_TRUE(parse_preset("0") == 0 && parse_preset("7") == 0 && parse_preset("") == 0) << "out-of-range presets rejected";
    EXPECT_EQ(parse_brightness("50"), 50) << "brightness payload";
    EXPECT_TRUE(parse_brightness("101") == -1 && parse_brightness("abc") == -1) << "invalid brightness rejected";
}

}  // namespace

