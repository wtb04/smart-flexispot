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

const char *active_preset();

/** The same thing named rather than slugged, for anywhere it is read by a
 *  person. */
const char *active_preset_label();

int preset_height_mm(int index);

}  // namespace desk
