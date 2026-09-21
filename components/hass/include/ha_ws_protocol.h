#pragma once

#include "cJSON.h"

#include <map>
#include <string>
#include <vector>

namespace hass::ws {

enum class MessageType {
    AuthRequired,
    AuthOk,
    AuthInvalid,
    Result,
    Event,
    Pong,
    Unknown,
};

MessageType classify(const cJSON *root);

/** The `id` a message carries, or -1 when it has none (auth messages). */
int message_id(const cJSON *root);

/** Carries no id, unlike every later message. */
std::string auth_message(const std::string &token);

/** Empty entity_ids returns "": omitting the list would subscribe to everything. */
std::string subscribe_entities_message(int id, const std::vector<std::string> &entity_ids);

std::string ping_message(int id);

std::string call_service_message(int id, const std::string &domain, const std::string &service,
                                 const std::string &entity_id);

/** Sends the value as a number when it parses as one: HA rejects "21.0" for 21.0. */
std::string call_service_message(int id, const std::string &domain, const std::string &service,
                                 const std::string &entity_id, const std::string &field,
                                 const std::string &value);

struct Entity {
    std::string                        state;
    std::string                        name;  // friendly_name
    std::string                        unit;  // unit_of_measurement
    std::map<std::string, std::string> attributes;
};

/** Fed from the compressed event format: "a" whole states, "c" diffs, "r" removals. */
class EntityStore {
public:
    /** Returns true if anything changed. */
    bool apply_event(const cJSON *event);

    const Entity *find(const std::string &entity_id) const;
    std::size_t   size() const { return entities_.size(); }

    const std::map<std::string, Entity> &all() const { return entities_; }

private:
    std::map<std::string, Entity> entities_;
};

}  // namespace hass::ws
