#pragma once

// The panel's own state, as last told: whether the owner's phone is here, the
// network, the time the bar shows and whether the screen is lit. What applies
// the updates writes it and publishes Topic::Status. On the LVGL task only.
namespace ui::detail {

struct StatusState {
    bool present   = false;  // the owner's phone is near, so their pages show
    bool wifi      = false;
    int  laptops   = 0;      // sharing through Desk Link
    bool screen_on = true;
    char time[16]  = "";     // HH:MM as the clock gives it, empty until it is set
    // The pack, and whether the panel runs on it: unplugged, on the desk, rather
    // than bolted at its edge on the mains.
    int  battery_percent = 0;
    bool on_battery      = false;
};

StatusState &status_state();

void status_take_battery(bool present, int percent, bool on_battery);

}  // namespace ui::detail
