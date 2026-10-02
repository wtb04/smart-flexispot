#pragma once

#include "ui.h"

// The panel's settings and its firmware, as last told or picked: the two-way
// choices, the notification volume, and an update on its way or waiting. The
// updates write them through the take_ functions, which publish Topic::Settings
// or Topic::Update; a choice picked on the page goes through settings_pick.
// On the LVGL task only.
namespace ui::detail {

struct SettingsState {
    bool        on[static_cast<int>(Setting::Count)] = {};
    int         notification_volume = -1;  // percent, -1 until told
    bool        update_known = false;
    UpdateState update{};
};

const SettingsState &settings_state();

void settings_take(int index, bool on);
void settings_take_volume(int percent);
void settings_take_update(const UpdateState &update);

/** Picked here: shown at once and sent on. */
void settings_pick(Setting setting, bool on);

}  // namespace ui::detail
