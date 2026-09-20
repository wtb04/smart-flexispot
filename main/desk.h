#pragma once

#include "esp_err.h"
#include "ui.h"

namespace desk {

/** Connects the UI to the control box and starts watching the link. */
esp_err_t start();

/** UI move handler: forwards a held button to the control box. */
void on_move(ui::Move direction);

/** UI preset handler: tap travels to a preset, hold stores the current height. */
void on_preset(int index, bool store);

/** Last reported height in millimetres, or negative if none yet. */
int height_mm();

/** True while the control box is talking to us. */
bool linked();

/** "idle", "moving_up" or "moving_down", as last commanded. */
const char *motion();

}  // namespace desk
