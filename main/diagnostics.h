#pragma once

#include "esp_err.h"
#include "ui.h"

#include <cstddef>

namespace diagnostics {
/** The diagnostics view's cards, for ui::set_cards before ui::init. */
const ui::Card *cards();
int             card_count();

/** Starts the task that fills the Setup page's diagnostics view. */
esp_err_t start();

/** Refreshes now rather than on the next tick. Safe from the LVGL task. */
void refresh();

/** Fills `out` with the recent log lines of one card, or of all of them when
 *  `card` is negative, oldest first, and returns how many. */
int logs(int card, bool warnings, ui::LogLine *out, int max);

/** Which channel the log buffer should keep a tag's lines in, and how many
 *  channels there are. Passed to logbuf::start(). */
int route(const char *tag);
int channels();

}  // namespace diagnostics
