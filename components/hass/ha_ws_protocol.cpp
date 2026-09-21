#include "ha_ws_protocol.h"

#include <cstdio>
#include <cstdlib>

namespace hass::ws {
namespace {

constexpr char KEY_STATE[]      = "s";
constexpr char KEY_ATTRIBUTES[] = "a";
constexpr char KEY_ADDITIONS[]  = "+";
constexpr char KEY_REMOVALS[]   = "-";

constexpr char ATTR_FRIENDLY_NAME[] = "friendly_name";
constexpr char ATTR_UNIT[]          = "unit_of_measurement";

std::string print_and_free(cJSON *root)
{
    char       *text = cJSON_PrintUnformatted(root);
    std::string out  = text != nullptr ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    return out;
}

std::string field(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

/** Attributes arrive as any JSON type; the panel only ever displays them. */
std::string as_text(const cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? "true" : "false";
    }
    if (cJSON_IsNumber(item)) {
        char buf[32];
        // Integers are the common case and look wrong with a trailing .0.
        if (item->valuedouble == static_cast<double>(item->valueint)) {
            std::snprintf(buf, sizeof(buf), "%d", item->valueint);
        } else {
            std::snprintf(buf, sizeof(buf), "%g", item->valuedouble);
        }
        return buf;
    }
    return "";
}

/** Two attributes are mirrored onto the entity, so they must stay in step. */
void hoist(Entity &entity, const std::string &key, const std::string &value)
{
    if (key == ATTR_FRIENDLY_NAME) {
        entity.name = value;
    } else if (key == ATTR_UNIT) {
        entity.unit = value;
    }
}

void unhoist(Entity &entity, const std::string &key)
{
    if (key == ATTR_FRIENDLY_NAME) {
        entity.name.clear();
    } else if (key == ATTR_UNIT) {
        entity.unit.clear();
    }
}

void merge_attributes(Entity &entity, const cJSON *attributes)
{
    if (!cJSON_IsObject(attributes)) {
        return;
    }
    for (const cJSON *item = attributes->child; item != nullptr; item = item->next) {
        if (item->string == nullptr) {
            continue;
        }
        const std::string value = as_text(item);
        entity.attributes[item->string] = value;
        hoist(entity, item->string, value);
    }
}

}  // namespace

MessageType classify(const cJSON *root)
{
    if (!cJSON_IsObject(root)) {
        return MessageType::Unknown;
    }
    const std::string type = field(root, "type");
    if (type == "auth_required") return MessageType::AuthRequired;
    if (type == "auth_ok") return MessageType::AuthOk;
    if (type == "auth_invalid") return MessageType::AuthInvalid;
    if (type == "result") return MessageType::Result;
    if (type == "event") return MessageType::Event;
    if (type == "pong") return MessageType::Pong;
    return MessageType::Unknown;
}

int message_id(const cJSON *root)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    return cJSON_IsNumber(id) ? id->valueint : -1;
}

std::string auth_message(const std::string &token)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "auth");
    cJSON_AddStringToObject(root, "access_token", token.c_str());
    return print_and_free(root);
}

std::string subscribe_entities_message(int id, const std::vector<std::string> &entity_ids)
{
    if (entity_ids.empty()) {
        // Omitting the list would subscribe to everything, which is not what an
        // empty list means to the caller.
        return "";
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "subscribe_entities");
    cJSON *ids = cJSON_CreateArray();
    for (const std::string &entity_id : entity_ids) {
        cJSON_AddItemToArray(ids, cJSON_CreateString(entity_id.c_str()));
    }
    cJSON_AddItemToObject(root, "entity_ids", ids);
    return print_and_free(root);
}

std::string ping_message(int id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "ping");
    return print_and_free(root);
}

std::string call_service_message(int id, const std::string &domain, const std::string &service,
                                 const std::string &entity_id)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "call_service");
    cJSON_AddStringToObject(root, "domain", domain.c_str());
    cJSON_AddStringToObject(root, "service", service.c_str());
    cJSON *target = cJSON_CreateObject();
    cJSON_AddStringToObject(target, "entity_id", entity_id.c_str());
    cJSON_AddItemToObject(root, "target", target);
    return print_and_free(root);
}

std::string call_service_message(int id, const std::string &domain, const std::string &service,
                                 const std::string &entity_id, const std::string &field,
                                 const std::string &value)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "type", "call_service");
    cJSON_AddStringToObject(root, "domain", domain.c_str());
    cJSON_AddStringToObject(root, "service", service.c_str());

    cJSON *data = cJSON_CreateObject();
    char  *end  = nullptr;
    const double number = std::strtod(value.c_str(), &end);
    // A setpoint has to arrive as a number; a mode has to arrive as a string.
    if (end != value.c_str() && *end == '\0') {
        cJSON_AddNumberToObject(data, field.c_str(), number);
    } else {
        cJSON_AddStringToObject(data, field.c_str(), value.c_str());
    }
    cJSON_AddItemToObject(root, "service_data", data);

    cJSON *target = cJSON_CreateObject();
    cJSON_AddStringToObject(target, "entity_id", entity_id.c_str());
    cJSON_AddItemToObject(root, "target", target);
    return print_and_free(root);
}

const Entity *EntityStore::find(const std::string &entity_id) const
{
    const auto it = entities_.find(entity_id);
    return it != entities_.end() ? &it->second : nullptr;
}

bool EntityStore::apply_event(const cJSON *event)
{
    if (!cJSON_IsObject(event)) {
        return false;
    }
    bool changed = false;

    // "a": whole states, sent once per entity when the subscription starts.
    const cJSON *added = cJSON_GetObjectItemCaseSensitive(event, "a");
    if (cJSON_IsObject(added)) {
        for (const cJSON *item = added->child; item != nullptr; item = item->next) {
            if (item->string == nullptr) {
                continue;
            }
            Entity entity;
            entity.state = field(item, KEY_STATE);
            merge_attributes(entity, cJSON_GetObjectItemCaseSensitive(item, KEY_ATTRIBUTES));
            entities_[item->string] = std::move(entity);
            changed                 = true;
        }
    }

    // "c": per-entity diffs.
    const cJSON *changes = cJSON_GetObjectItemCaseSensitive(event, "c");
    if (cJSON_IsObject(changes)) {
        for (const cJSON *item = changes->child; item != nullptr; item = item->next) {
            if (item->string == nullptr) {
                continue;
            }
            Entity &entity = entities_[item->string];

            const cJSON *additions = cJSON_GetObjectItemCaseSensitive(item, KEY_ADDITIONS);
            if (cJSON_IsObject(additions)) {
                const cJSON *state = cJSON_GetObjectItemCaseSensitive(additions, KEY_STATE);
                if (cJSON_IsString(state) && state->valuestring != nullptr) {
                    entity.state = state->valuestring;
                    changed      = true;
                }
                const cJSON *attrs = cJSON_GetObjectItemCaseSensitive(additions, KEY_ATTRIBUTES);
                if (cJSON_IsObject(attrs)) {
                    merge_attributes(entity, attrs);
                    changed = true;
                }
            }

            // "-": attributes Home Assistant has dropped, as a list of names.
            // Without this they linger forever -- a light keeps a brightness it
            // no longer has, a sensor keeps a unit it stopped reporting.
            const cJSON *removals = cJSON_GetObjectItemCaseSensitive(item, KEY_REMOVALS);
            if (cJSON_IsObject(removals)) {
                const cJSON *gone = cJSON_GetObjectItemCaseSensitive(removals, KEY_ATTRIBUTES);
                if (cJSON_IsArray(gone)) {
                    for (const cJSON *name = gone->child; name != nullptr; name = name->next) {
                        if (!cJSON_IsString(name) || name->valuestring == nullptr) {
                            continue;
                        }
                        entity.attributes.erase(name->valuestring);
                        unhoist(entity, name->valuestring);
                        changed = true;
                    }
                }
            }
        }
    }

    // "r": entities that have left the state machine, as a list of ids.
    const cJSON *removed = cJSON_GetObjectItemCaseSensitive(event, "r");
    if (cJSON_IsArray(removed)) {
        for (const cJSON *name = removed->child; name != nullptr; name = name->next) {
            if (cJSON_IsString(name) && name->valuestring != nullptr) {
                changed |= entities_.erase(name->valuestring) > 0;
            }
        }
    }

    return changed;
}

}  // namespace hass::ws
