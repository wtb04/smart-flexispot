#pragma once

#include "ui.h"

// What only the panel's own hardware can say: the desk through its control
// box, the battery, the phone seen over Bluetooth, and the focus timer's
// clock. Played here, and switched from the keyboard to see each state.
namespace hardware {
/** After ui::init: shows where everything stands. */
void start();

/** Once per frame. */
void tick();

void on_move(ui::Move direction);
void on_preset(int index, bool store);
void on_focus(ui::FocusAction action);
void on_focus_plan(int work_min, int break_min, int long_break_min, int rounds);

void end_focus_part();  // the part under way runs out now
void toggle_desk_link();
void next_battery();
void toggle_phone();
void toggle_wifi();

/** Whether Home Assistant answers, which the rail shows beside Wi-Fi. */
void set_home_assistant(bool up);
}  // namespace hardware
