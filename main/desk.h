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

}  // namespace desk
