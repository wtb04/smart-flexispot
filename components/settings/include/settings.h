#pragma once

#include "esp_err.h"

#include <cstdint>

namespace settings {

enum class Key : std::uint8_t {
    Brightness,
    Charging,
    Volume,
    PresenceGate,
    DeskBluetooth,
    Count,
};

/** Reads what was stored. Call once, before anything asks for a value. */
esp_err_t load();

int get(Key key);

bool enabled(Key key);

/** Takes effect at once and is written back shortly after; a run of changes,
 *  such as a slider being dragged, costs one write rather than one per step. */
void set(Key key, int value);

}  // namespace settings
