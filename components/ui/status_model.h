#pragma once

// The panel's own state, as last told: whether the owner's phone is here, the
// network, the time the rail shows and whether the screen is lit. What applies
// the updates writes it and publishes Topic::Status. On the LVGL task only.
namespace ui::detail {

struct StatusState {
    bool present   = false;  // the owner's phone is near, so their pages show
    bool wifi      = false;
    bool screen_on = true;
    char time[16]  = "";     // HH:MM as the clock gives it, empty until it is set
};

StatusState &status_state();

}  // namespace ui::detail
