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

}  // namespace desk
