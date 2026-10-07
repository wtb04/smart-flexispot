#pragma once

#include "ui.h"

#include <cstdint>

// What the panel reports of itself, as last told: the diagnostics cards with
// their rows, and the glances on the setup page. The updates write them and
// publish Topic::Diagnostics; each carries a stamp, so a view draws only what
// changed since. On the LVGL task only.
namespace ui::detail {

struct InfoState {
    char          value[64] = "";
    Level         level     = Level::Neutral;
    std::uint32_t stamp     = 0;  // when it was last told, 0 never
};

struct DiagnosticsState {
    InfoState cards[kMaxCards];
    InfoState rows[kMaxCards][kMaxRows];
    InfoState glances[kGlanceCount];
};

const DiagnosticsState &diagnostics_state();

void diagnostics_take_card(int card, const char *value, Level level);
void diagnostics_take_row(int card, int row, const char *value, Level level);
void diagnostics_take_glance(int index, const char *value);

}  // namespace ui::detail
