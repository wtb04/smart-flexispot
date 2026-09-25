#include "ha_ws_protocol.h"

#include <cstdio>
#include <string>

namespace {
using namespace hass::ws;

int g_failures = 0;

void check(bool ok, const char *what)
{
    std::printf("%-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

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

void test_handshake()
{
    check(classify_text(R"({"type":"auth_required","ha_version":"2026.9"})") ==
              MessageType::AuthRequired,
          "auth_required recognised");
    check(classify_text(R"({"type":"auth_ok"})") == MessageType::AuthOk, "auth_ok recognised");
    check(classify_text(R"({"type":"auth_invalid","message":"nope"})") == MessageType::AuthInvalid,
          "auth_invalid recognised");
    check(classify_text(R"({"id":2,"type":"event","event":{}})") == MessageType::Event,
          "event recognised");
    check(classify_text(R"({"type":"something_new"})") == MessageType::Unknown,
          "unknown type does not throw");

    const std::string auth = auth_message("abc123");
    check(auth.find("\"type\":\"auth\"") != std::string::npos, "auth message type");
    check(auth.find("\"access_token\":\"abc123\"") != std::string::npos, "auth carries the token");
    check(auth.find("\"id\"") == std::string::npos, "auth carries no id");
}

void test_subscription()
{
    const std::string sub = subscribe_entities_message(2, {"light.office", "sensor.outside"});
    check(sub.find("\"type\":\"subscribe_entities\"") != std::string::npos, "subscribe type");
    check(sub.find("\"id\":2") != std::string::npos, "subscribe carries its id");
    check(sub.find("light.office") != std::string::npos, "entity ids included");

    check(subscribe_entities_message(3, {}).empty(), "empty list does not subscribe to everything");
}

void test_entity_store_add()
{
    EntityStore store;
    check(feed(store, R"({"id":2,"type":"event","event":{"a":{
            "light.office":{"s":"on","a":{"friendly_name":"Office","brightness":180}},
            "sensor.temp":{"s":"21.5","a":{"unit_of_measurement":"C"}}}}})"),
          "initial states applied");
    check(store.size() == 2, "both entities stored");

    const Entity *light = store.find("light.office");
    check(light != nullptr && light->state == "on", "state stored");
    check(light != nullptr && light->name == "Office", "friendly_name hoisted to name");
    check(light != nullptr && light->attributes.at("brightness") == "180",
          "numeric attribute stringified without a trailing .0");

    const Entity *sensor = store.find("sensor.temp");
    check(sensor != nullptr && sensor->unit == "C", "unit hoisted");
}

void test_entity_store_change()
{
    EntityStore store;
    feed(store, R"({"event":{"a":{"light.office":{"s":"on","a":{"brightness":180,
          "friendly_name":"Office","color_temp":370}}}}})");

    check(feed(store, R"({"event":{"c":{"light.office":{"+":{"s":"off"}}}}})"),
          "state change applied");
    check(store.find("light.office")->state == "off", "new state stored");
    check(store.find("light.office")->attributes.at("brightness") == "180",
          "untouched attributes survive a diff");

    check(feed(store, R"({"event":{"c":{"light.office":{"-":{"a":["brightness","color_temp"]}}}}})"),
          "removals applied");
    const Entity *light = store.find("light.office");
    check(light->attributes.count("brightness") == 0, "cleared attribute removed");
    check(light->attributes.count("color_temp") == 0, "second cleared attribute removed");
    check(light->attributes.count("friendly_name") == 1, "untouched attribute kept");

    feed(store, R"({"event":{"c":{"light.office":{"-":{"a":["friendly_name"]}}}}})");
    check(store.find("light.office")->name.empty(), "cleared friendly_name clears the name");

    feed(store, R"({"event":{"a":{"sensor.temp":{"s":"9","a":{"unit_of_measurement":"C"}}}}})");
    feed(store, R"({"event":{"c":{"sensor.temp":{"-":{"a":["unit_of_measurement"]}}}}})");
    check(store.find("sensor.temp")->unit.empty(), "cleared unit clears the unit");
}

void test_entity_store_remove()
{
    EntityStore store;
    feed(store, R"({"event":{"a":{"light.a":{"s":"on"},"light.b":{"s":"off"}}}})");
    check(feed(store, R"({"event":{"r":["light.a"]}})"), "removal applied");
    check(store.size() == 1 && store.find("light.a") == nullptr, "entity gone");
    check(store.find("light.b") != nullptr, "the other entity survives");
}

void test_entity_store_filter()
{
    EntityStore store;
    store.keep_attributes({"volume_level", "media_title"});
    feed(store, R"({"event":{"a":{"media_player.x":{"s":"playing","a":{
        "friendly_name":"Speaker","media_title":"Song","volume_level":0.4,
        "source_list":["a","b"],"supported_features":152461}}}}})");
    const Entity *x = store.find("media_player.x");
    check(x != nullptr && x->attributes.size() == 2, "only the kept attributes stay");
    check(x != nullptr && x->name == "Speaker", "the name is kept all the same");
    feed(store, R"({"event":{"c":{"media_player.x":{"+":{"a":{"media_title":"Next",
        "entity_picture":"/x.jpg"}}}}}})");
    check(x->attributes.count("entity_picture") == 0 && x->attributes.at("media_title") == "Next",
          "a change is filtered the same way");
}

void test_malformed()
{
    EntityStore store;
    check(!feed(store, R"({"event":{}})"), "empty event changes nothing");
    check(!feed(store, R"({"event":{"a":"not an object"}})"), "wrong type ignored");
    check(!feed(store, R"({"event":{"r":"not an array"}})"), "wrong removal type ignored");
    feed(store, R"({"event":{"c":{"light.ghost":{"+":{"s":"on"}}}}})");
    check(store.find("light.ghost") != nullptr, "diff for an unknown entity is tolerated");
}

void test_service_call()
{
    const std::string call = call_service_message(7, "light", "turn_on", "light.office");
    check(call.find("\"type\":\"call_service\"") != std::string::npos, "service call type");
    check(call.find("\"domain\":\"light\"") != std::string::npos, "domain");
    check(call.find("\"service\":\"turn_on\"") != std::string::npos, "service");
    check(call.find("\"entity_id\":\"light.office\"") != std::string::npos, "target entity");
}

void test_service_call_with_data()
{
    const std::string setpoint =
        call_service_message(9, "climate", "set_temperature", "climate.x", "temperature", "21.5");
    check(setpoint.find("\"temperature\":21.5") != std::string::npos,
          "numeric setpoint sent as a number");

    const std::string mode =
        call_service_message(10, "climate", "set_hvac_mode", "climate.x", "hvac_mode", "heat");
    check(mode.find("\"hvac_mode\":\"heat\"") != std::string::npos, "mode sent as a string");

    const std::string mixed =
        call_service_message(11, "climate", "set_temperature", "climate.x", "temperature", "21.5C");
    check(mixed.find("\"temperature\":\"21.5C\"") != std::string::npos,
          "partially numeric value stays a string");
}

void test_requests()
{
    const std::string sent = numbered(12, R"({"type":"config_entries/get","domain":"x"})");
    check(sent.find("\"id\":12") != std::string::npos, "request numbered as it goes out");
    check(sent.find("\"domain\":\"x\"") != std::string::npos, "request keeps its body");
    check(numbered(13, R"({"id":4,"type":"ping"})").find("\"id\":13") != std::string::npos,
          "a stale number is replaced");
    check(numbered(14, "[1]").empty() && numbered(15, "not json").empty(),
          "only an object can be sent");

    cJSON *ok = cJSON_Parse(R"({"id":12,"type":"result","success":true,"result":{"response":1}})");
    const cJSON *result = reply_result(ok);
    check(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(result, "response")),
          "a reply's result is handed over");
    cJSON_Delete(ok);

    cJSON *bad = cJSON_Parse(R"({"id":12,"type":"result","success":false,"error":{}})");
    check(reply_result(bad) == nullptr, "a failed reply has no result");
    cJSON_Delete(bad);
}

}  // namespace

int main()
{
    test_handshake();
    test_subscription();
    test_entity_store_add();
    test_entity_store_change();
    test_entity_store_remove();
    test_entity_store_filter();
    test_malformed();
    test_service_call();
    test_service_call_with_data();
    test_requests();
    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
