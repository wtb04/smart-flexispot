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
    Accent,
    RailSide,
    Flipped,
    OrientAuto,  // follow the IMU rather than Flipped
    OrientSign,  // which way along the IMU's x gravity points with the screen upright, -1 or 1
    FocusWork,   // minutes of the focus timer's parts, and rounds before the long break
    FocusBreak,
    FocusLong,
    FocusRounds,
    FocusPhase,    // the timer as it stood, kept across a restart: see focus::Saved
    FocusRound,
    FocusRunning,
    FocusLeft,     // milliseconds
    FocusLength,
    FocusEnds,     // seconds since the epoch, 0 when not running or not known
    Count,
};

/** Reads what was stored. Call once, before anything asks for a value. */
esp_err_t load();

int get(Key key);

bool enabled(Key key);

/** Takes effect at once and is written back shortly after; a run of changes,
 *  such as a slider being dragged, costs one write rather than one per step. */
void set(Key key, int value);

/** Writes back now whatever is waiting, for when the panel is about to restart. */
void flush();

}  // namespace settings
