#include "status_model.h"

#include "topics.h"

namespace ui::detail {

StatusState &status_state()
{
    static StatusState state;
    return state;
}

void status_take_battery(bool present, int percent, bool on_battery)
{
    StatusState &status    = status_state();
    status.battery_percent = percent;
    status.on_battery      = present && on_battery;
    publish(Topic::Status);
}

}  // namespace ui::detail
