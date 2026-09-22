#include "battery.h"
#include "ble.h"
#include "board.h"
#include "desk.h"
#include "diagnostics.h"
#include "esp_log.h"
#include "esp_system.h"
#include "logbuf.h"
#include "nvs_flash.h"
#include "power.h"
#include "backup_clock.h"
#include "radar.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "sound.h"
#include "room.h"
#include "telemetry.h"
#include "ui.h"
#include "wallclock.h"
#include "wifi.h"

namespace {
constexpr char TAG[] = "tab5";

// Both the panel's slider and Home Assistant land here, so the published value
// stays right either way.
void on_brightness_changed(int percent)
{
    board::set_brightness_percent(percent);
    telemetry::note_brightness(percent);
    settings::set(settings::Key::Brightness, percent);
}

// The panel has already redrawn itself and applied whatever it owns; this side
// stores the choice and applies whatever it does not.
void on_setting(ui::Setting setting, bool on)
{
    switch (setting) {
        case ui::Setting::Charging:
            settings::set(settings::Key::Charging, on);
            ESP_ERROR_CHECK_WITHOUT_ABORT(power::set_charging(on));
            break;
        case ui::Setting::PresenceGate:
            settings::set(settings::Key::PresenceGate, on);
            break;
        case ui::Setting::DeskBluetooth:
            settings::set(settings::Key::DeskBluetooth, on);
            break;
        default:
            break;
    }
}

// Storing on every step would be a flash write per pixel dragged; the store
// coalesces, so this only has to keep the codec and the setting in step. The
// preview is the point of the slider -- you hear what you are choosing.
void on_volume(int percent, bool preview)
{
    sound::set_volume(percent);
    settings::set(settings::Key::Volume, percent);
    if (preview) {
        sound::ding();
    }
}

void on_primary(std::uint32_t colour)
{
    settings::set(settings::Key::Accent, static_cast<int>(colour));
}

void on_rail_side(bool right)
{
    settings::set(settings::Key::RailSide, right);
}

void on_orientation(bool flipped)
{
    settings::set(settings::Key::Flipped, flipped);
}

void on_radar_page(bool showing, bool reachable)
{
    radar::set_enabled(reachable);
    radar::set_active(showing);
}

void on_restart()
{
    ESP_LOGI(TAG, "restart requested from the panel");
    // The panel goes on scanning out whatever the MIPI link last left in it
    // while the SoC restarts, which is the flash of blue. Dimming is not
    // enough -- set_brightness floors at the lowest level the panel honours --
    // so the backlight goes off and the panel is put to sleep.
    ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_off());
    esp_restart();
}
}  // namespace

extern "C" void app_main(void)
{
    // Before anything that stores settings: the desk keeps its learned preset
    // heights here, and Wi-Fi its calibration data.
    if (esp_err_t err = nvs_flash_init();
        err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_init());
    }
    // First, so the diagnostics pages can show what happened during startup.
    ESP_ERROR_CHECK_WITHOUT_ABORT(logbuf::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(settings::load());

    const bool flipped = settings::enabled(settings::Key::Flipped);
    ESP_ERROR_CHECK(board::init(flipped));
    const ui::Handlers handlers{desk::on_move,      desk::on_preset,      on_brightness_changed,
                                room::on_media,     room::on_setpoint,    room::on_mode,
                                room::on_lights,    room::on_light,       room::on_dial_toggle,
                                diagnostics::refresh, on_setting,         on_volume,
                                on_restart,           on_radar_page,      diagnostics::logs,
                                on_primary,           on_rail_side,       on_orientation};
    const int brightness = settings::get(settings::Key::Brightness);
    ESP_ERROR_CHECK(ui::init(handlers, brightness,
                             static_cast<std::uint32_t>(settings::get(settings::Key::Accent)),
                             settings::enabled(settings::Key::RailSide), flipped));
    // Through the same path a change takes, so the panel, the backlight and
    // what gets published all start out agreeing.
    on_brightness_changed(brightness);
    // Only now is there something worth lighting.
    ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_on(brightness));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_setting(ui::Setting::Charging, settings::enabled(settings::Key::Charging)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_setting(
        ui::Setting::PresenceGate, settings::enabled(settings::Key::PresenceGate)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_setting(
        ui::Setting::DeskBluetooth, settings::enabled(settings::Key::DeskBluetooth)));
    const int volume = settings::get(settings::Key::Volume);
    sound::set_volume(volume);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_notification_volume(volume));

    room::init();
    // Neither is fatal: panicking here put the panel in a boot loop over a
    // peripheral it can perfectly well run without.
    ESP_ERROR_CHECK_WITHOUT_ABORT(desk::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("desk", 25));
    // Before the network, because the whole point is that the clock is right
    // on the first frame rather than whenever the network answers.
    ESP_ERROR_CHECK_WITHOUT_ABORT(rtc::start());

    ESP_ERROR_CHECK_WITHOUT_ABORT(battery::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(sound::init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("power", 45));
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi::start());
    ESP_ERROR_CHECK(wallclock::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("network", 70));
    ESP_ERROR_CHECK_WITHOUT_ABORT(telemetry::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(diagnostics::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("home assistant", 85));

    ESP_LOGI(TAG, "up");
    // app_main returns; the LVGL, loctek and supervisor tasks carry on.
}
