#include "ha_ws_protocol.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

namespace {
using namespace hass::ws;

/** Applies a raw event payload, as it would arrive on the socket. */
bool feed(EntityStore &store, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == nullptr) {
        return false;
    }
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const bool   out   = store.apply_event(event != nullptr ? event : root);
    cJSON_Delete(root);
    return out;
}

MessageType classify_text(const char *json)
{
    cJSON            *root = cJSON_Parse(json);
    const MessageType out  = classify(root);
    cJSON_Delete(root);
    return out;
}

TEST(HaWsProtocol, handshake)
{
    EXPECT_EQ(classify_text(R"({"type":"auth_required","ha_version":"2026.9"})"), MessageType::AuthRequired) << "auth_required recognised";
    EXPECT_EQ(classify_text(R"({"type":"auth_ok"})"), MessageType::AuthOk) << "auth_ok recognised";
    EXPECT_EQ(classify_text(R"({"type":"auth_invalid","message":"nope"})"), MessageType::AuthInvalid) << "auth_invalid recognised";
    EXPECT_EQ(classify_text(R"({"id":2,"type":"event","event":{}})"), MessageType::Event) << "event recognised";
    EXPECT_EQ(classify_text(R"({"type":"something_new"})"), MessageType::Unknown) << "unknown type does not throw";

    const std::string auth = auth_message("abc123");
    EXPECT_NE(auth.find("\"type\":\"auth\""), std::string::npos) << "auth message type";
    EXPECT_NE(auth.find("\"access_token\":\"abc123\""), std::string::npos) << "auth carries the token";
    EXPECT_EQ(auth.find("\"id\""), std::string::npos) << "auth carries no id";
}

TEST(HaWsProtocol, subscription)
{
    const std::string sub = subscribe_entities_message(2, {"light.office", "sensor.outside"});
    EXPECT_NE(sub.find("\"type\":\"subscribe_entities\""), std::string::npos) << "subscribe type";
    EXPECT_NE(sub.find("\"id\":2"), std::string::npos) << "subscribe carries its id";
    EXPECT_NE(sub.find("light.office"), std::string::npos) << "entity ids included";

    EXPECT_TRUE(subscribe_entities_message(3, {}).empty()) << "empty list does not subscribe to everything";
}

TEST(HaWsProtocol, entity_store_add)
{
    EntityStore store;
    EXPECT_TRUE(feed(store, R"({"id":2,"type":"event","event":{"a":{
            "light.office":{"s":"on","a":{"friendly_name":"Office","brightness":180}},
            "sensor.temp":{"s":"21.5","a":{"unit_of_measurement":"C"}}}}})")) << "initial states applied";
    EXPECT_EQ(store.size(), 2) << "both entities stored";

    const Entity *light = store.find("light.office");
    EXPECT_TRUE(light != nullptr && light->state == "on") << "state stored";
    EXPECT_TRUE(light != nullptr && light->name == "Office") << "friendly_name hoisted to name";
    EXPECT_TRUE(light != nullptr && light->attributes.at("brightness") == "180") << "numeric attribute stringified without a trailing .0";

    const Entity *sensor = store.find("sensor.temp");
    EXPECT_TRUE(sensor != nullptr && sensor->unit == "C") << "unit hoisted";
}

TEST(HaWsProtocol, entity_store_change)
{
    EntityStore store;
    feed(store, R"({"event":{"a":{"light.office":{"s":"on","a":{"brightness":180,
          "friendly_name":"Office","color_temp":370}}}}})");

    EXPECT_TRUE(feed(store, R"({"event":{"c":{"light.office":{"+":{"s":"off"}}}}})")) << "state change applied";
    EXPECT_TRUE(store.find("light.office")->state == "off") << "new state stored";
    EXPECT_TRUE(store.find("light.office")->attributes.at("brightness") == "180") << "untouched attributes survive a diff";

    EXPECT_TRUE(feed(store, R"({"event":{"c":{"light.office":{"-":{"a":["brightness","color_temp"]}}}}})")) << "removals applied";
    const Entity *light = store.find("light.office");
    EXPECT_TRUE(light->attributes.count("brightness") == 0) << "cleared attribute removed";
    EXPECT_TRUE(light->attributes.count("color_temp") == 0) << "second cleared attribute removed";
    EXPECT_TRUE(light->attributes.count("friendly_name") == 1) << "untouched attribute kept";

    feed(store, R"({"event":{"c":{"light.office":{"-":{"a":["friendly_name"]}}}}})");
    EXPECT_TRUE(store.find("light.office")->name.empty()) << "cleared friendly_name clears the name";

    feed(store, R"({"event":{"a":{"sensor.temp":{"s":"9","a":{"unit_of_measurement":"C"}}}}})");
    feed(store, R"({"event":{"c":{"sensor.temp":{"-":{"a":["unit_of_measurement"]}}}}})");
    EXPECT_TRUE(store.find("sensor.temp")->unit.empty()) << "cleared unit clears the unit";
}

TEST(HaWsProtocol, entity_store_remove)
{
    EntityStore store;
    feed(store, R"({"event":{"a":{"light.a":{"s":"on"},"light.b":{"s":"off"}}}})");
    EXPECT_TRUE(feed(store, R"({"event":{"r":["light.a"]}})")) << "removal applied";
    EXPECT_TRUE(store.size() == 1 && store.find("light.a") == nullptr) << "entity gone";
    EXPECT_NE(store.find("light.b"), nullptr) << "the other entity survives";
}

TEST(HaWsProtocol, entity_store_filter)
{
    EntityStore store;
    store.keep_attributes({"volume_level", "media_title"});
    feed(store, R"({"event":{"a":{"media_player.x":{"s":"playing","a":{
        "friendly_name":"Speaker","media_title":"Song","volume_level":0.4,
        "source_list":["a","b"],"supported_features":152461}}}}})");
    const Entity *x = store.find("media_player.x");
    EXPECT_TRUE(x != nullptr && x->attributes.size() == 2) << "only the kept attributes stay";
    EXPECT_TRUE(x != nullptr && x->name == "Speaker") << "the name is kept all the same";
    feed(store, R"({"event":{"c":{"media_player.x":{"+":{"a":{"media_title":"Next",
        "entity_picture":"/x.jpg"}}}}}})");
    EXPECT_TRUE(x->attributes.count("entity_picture") == 0 && x->attributes.at("media_title") == "Next") << "a change is filtered the same way";
}

TEST(HaWsProtocol, malformed)
{
    EntityStore store;
    EXPECT_TRUE(!feed(store, R"({"event":{}})")) << "empty event changes nothing";
    EXPECT_TRUE(!feed(store, R"({"event":{"a":"not an object"}})")) << "wrong type ignored";
    EXPECT_TRUE(!feed(store, R"({"event":{"r":"not an array"}})")) << "wrong removal type ignored";
    feed(store, R"({"event":{"c":{"light.ghost":{"+":{"s":"on"}}}}})");
    EXPECT_NE(store.find("light.ghost"), nullptr) << "diff for an unknown entity is tolerated";
}

TEST(HaWsProtocol, service_call)
{
    const std::string call = call_service_message(7, "light", "turn_on", "light.office");
    EXPECT_NE(call.find("\"type\":\"call_service\""), std::string::npos) << "service call type";
    EXPECT_NE(call.find("\"domain\":\"light\""), std::string::npos) << "domain";
    EXPECT_NE(call.find("\"service\":\"turn_on\""), std::string::npos) << "service";
    EXPECT_NE(call.find("\"entity_id\":\"light.office\""), std::string::npos) << "target entity";
}

TEST(HaWsProtocol, service_call_with_data)
{
    const std::string setpoint =
        call_service_message(9, "climate", "set_temperature", "climate.x", "temperature", "21.5");
    EXPECT_NE(setpoint.find("\"temperature\":21.5"), std::string::npos) << "numeric setpoint sent as a number";

    const std::string mode =
        call_service_message(10, "climate", "set_hvac_mode", "climate.x", "hvac_mode", "heat");
    EXPECT_NE(mode.find("\"hvac_mode\":\"heat\""), std::string::npos) << "mode sent as a string";

    const std::string mixed =
        call_service_message(11, "climate", "set_temperature", "climate.x", "temperature", "21.5C");
    EXPECT_NE(mixed.find("\"temperature\":\"21.5C\""), std::string::npos) << "partially numeric value stays a string";
}

TEST(HaWsProtocol, requests)
{
    const std::string sent = numbered(12, R"({"type":"config_entries/get","domain":"x"})");
    EXPECT_NE(sent.find("\"id\":12"), std::string::npos) << "request numbered as it goes out";
    EXPECT_NE(sent.find("\"domain\":\"x\""), std::string::npos) << "request keeps its body";
    EXPECT_NE(numbered(13, R"({"id":4,"type":"ping"})").find("\"id\":13"), std::string::npos) << "a stale number is replaced";
    EXPECT_TRUE(numbered(14, "[1]").empty() && numbered(15, "not json").empty()) << "only an object can be sent";

    cJSON *ok = cJSON_Parse(R"({"id":12,"type":"result","success":true,"result":{"response":1}})");
    const cJSON *result = reply_result(ok);
    EXPECT_TRUE(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(result, "response"))) << "a reply's result is handed over";
    cJSON_Delete(ok);

    cJSON *bad = cJSON_Parse(R"({"id":12,"type":"result","success":false,"error":{}})");
    EXPECT_EQ(reply_result(bad), nullptr) << "a failed reply has no result";
    cJSON_Delete(bad);
}

}  // namespace

