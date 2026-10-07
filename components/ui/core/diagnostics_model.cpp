#include "diagnostics_model.h"

#include "topics.h"

#include <cstdio>

namespace ui::detail {
namespace {
DiagnosticsState s_diagnostics;
std::uint32_t    s_stamp = 0;

void tell(InfoState &info, const char *value, Level level)
{
    std::snprintf(info.value, sizeof(info.value), "%s", value != nullptr ? value : "");
    info.level = level;
    info.stamp = ++s_stamp;
    publish(Topic::Diagnostics);
}
}  // namespace

const DiagnosticsState &diagnostics_state()
{
    return s_diagnostics;
}

void diagnostics_take_card(int card, const char *value, Level level)
{
    tell(s_diagnostics.cards[card], value, level);
}

void diagnostics_take_row(int card, int row, const char *value, Level level)
{
    tell(s_diagnostics.rows[card][row], value, level);
}

void diagnostics_take_glance(int index, const char *value)
{
    tell(s_diagnostics.glances[index], value, Level::Neutral);
}

}  // namespace ui::detail
