#pragma once

#include "esp_err.h"
#include "ui.h"

namespace desk {
esp_err_t start();

void on_move(ui::Move direction);

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
