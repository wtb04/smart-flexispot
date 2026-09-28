#pragma once

#include <string>

// Notices as Home Assistant sends them to the panel: the same payload, read by
// the same parser, shown as network.cpp shows them. N steps through a few, and
// sim/send.sh sends any other to the simulator on this Mac.
namespace notices {
/** The payload as it would come in on the notify topic: JSON, or plain text. */
void from_home_assistant(const std::string &payload);

/** One of a few typical ones, the next each time. */
void next_example();

/** Listens for sim/send.sh on localhost. */
void listen();

/** Once per frame: whatever send.sh sent. */
void pump();
}  // namespace notices
