#pragma once

#include "esp_err.h"

#include <cstdint>

namespace desk {
enum class Move : std::int8_t {
    Down = -1,
    Stop = 0,
    Up   = 1,
};

enum class Link : std::uint8_t {
    Wire,       // the UART on this board
    Bluetooth,  // through the companion
};

/** What a notice is: something done, or something to know. */
enum class Tone : std::uint8_t { Done, Hint };

/** How the desk shows itself. Each may be null. Called from the desk's own
 *  tasks, so each must return at once. */
struct View {
    void (*preset_active)(int index, bool active);
    void (*height)(int height_mm);  // negative when not known
    void (*available)(bool linked);
    void (*notice)(const char *message, Tone tone, int timeout_ms);
};

esp_err_t start(Link link, const View &view);

void on_move(Move direction);

/** A move asked for over the network is a hold with nobody's finger on it, so
 *  it is let go of after a moment unless asked for again. */
void on_network_move(Move direction);

/** Tap travels to a preset, hold stores the current height. */
void on_preset(int index, bool store);

/** Millimetres, or negative if nothing has been reported yet. */
int height_mm();

bool linked();

/** "idle", "moving_up" or "moving_down", as last commanded. */
const char *motion();

/** Which preset the desk is standing at, or negative when between. */
int active_preset_index();

/** "Preset 1" to "Preset 6", or "Between": what Home Assistant is told. The
 *  screen has its own names for them. */
const char *active_preset_label();

int preset_height_mm(int index);

}  // namespace desk
