#include "settings_model.h"

#include "topics.h"
#include "ui_internal.h"

namespace ui::detail {
namespace {
// Until the stored settings come, the owner's pages wait for the phone.
SettingsState initial()
{
    SettingsState settings;
    settings.on[static_cast<int>(Setting::PresenceGate)] = true;
    return settings;
}
SettingsState s_settings = initial();
}  // namespace

const SettingsState &settings_state()
{
    return s_settings;
}

void settings_take(int index, bool on)
{
    s_settings.on[index] = on;
    publish(Topic::Settings);
}

void settings_take_volume(int percent)
{
    s_settings.notification_volume = percent;
    publish(Topic::Settings);
}

void settings_take_update(const UpdateState &update)
{
    s_settings.update       = update;
    s_settings.update_known = true;
    publish(Topic::Update);
}

void settings_pick(Setting setting, bool on)
{
    const int index = static_cast<int>(setting);
    if (on == s_settings.on[index]) {
        return;
    }
    settings_take(index, on);
    if (s_handlers.setting != nullptr) {
        s_handlers.setting(setting, on);
    }
}

}  // namespace ui::detail
