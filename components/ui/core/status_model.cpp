#include "status_model.h"

#include "topics.h"

namespace ui::detail {

StatusState &status_state()
{
    static StatusState state;
    return state;
}

void status_take_battery(bool present, int percent, bool charging, bool on_battery)
{
    StatusState &status    = status_state();
    status.battery_present = present;
    status.battery_percent = percent;
    status.charging        = charging;
    status.on_battery      = present && on_battery;
    publish(Topic::Status);
}

void status_splash_gone()
{
    status_state().splash_gone = true;
    publish(Topic::Status);
}

}  // namespace ui::detail
