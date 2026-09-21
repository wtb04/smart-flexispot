#pragma once

#include "cJSON.h"

#include <map>
#include <string>
#include <vector>

// The Home Assistant WebSocket API, with no transport in sight so it can be
// tested on a host. Handshake, message builders, and the entity store fed by
// the compressed event stream.
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

/** Sent in reply to auth_required. Carries no id, unlike every later message. */
std::string auth_message(const std::string &token);

/**
 * @brief Subscribes to state changes for a specific set of entities.
 *
 * Omitting entity_ids subscribes to the entire state machine, which on a real
 * installation is a several-hundred-kilobyte dump, so an empty list returns an
 * empty string rather than quietly asking for everything.
 */
std::string subscribe_entities_message(int id, const std::vector<std::string> &entity_ids);

std::string ping_message(int id);

/** Calls a service, e.g. domain "light", service "turn_on". */
std::string call_service_message(int id, const std::string &domain, const std::string &service,
                                 const std::string &entity_id);

/**
 * @brief A service call carrying one field, such as a setpoint or a mode.
 *
 * The value is sent as a number when it parses as one and as a string
 * otherwise, because Home Assistant rejects "21.0" where it expects 21.0.
 */
std::string call_service_message(int id, const std::string &domain, const std::string &service,
                                 const std::string &entity_id, const std::string &field,
                                 const std::string &value);

struct Entity {
    std::string                        state;
    std::string                        name;  // friendly_name, if it has one
    std::string                        unit;  // unit_of_measurement, if it has one
    std::map<std::string, std::string> attributes;
};

/**
 * @brief The panel's copy of the entities it subscribed to.
 *
 * Fed from the compressed event format: "a" adds whole states, "c" carries
 * per-entity diffs with "+" for set fields and "-" for cleared ones, and "r"
 * lists entities that have gone away.
 */
class EntityStore {
public:
    /** Applies one `event` object. Returns true if anything changed. */
    bool apply_event(const cJSON *event);

    const Entity *find(const std::string &entity_id) const;
    std::size_t   size() const { return entities_.size(); }

    const std::map<std::string, Entity> &all() const { return entities_; }

private:
    std::map<std::string, Entity> entities_;
};

}  // namespace hass::ws
