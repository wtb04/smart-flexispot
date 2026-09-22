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

void on_brightness_changed(int percent)
{
    board::set_brightness_percent(percent);
    telemetry::note_brightness(percent);
    settings::set(settings::Key::Brightness, percent);
}

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

void on_screen(bool on)
{
    telemetry::note_screen(on);
    if (on) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_on(settings::get(settings::Key::Brightness)));
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_off());
    }
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_off());
    esp_restart();
}
}  // namespace

extern "C" void app_main(void)
{
    if (esp_err_t err = nvs_flash_init();
        err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_init());
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(logbuf::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(settings::load());

    const bool flipped = settings::enabled(settings::Key::Flipped);
    ESP_ERROR_CHECK(board::init(flipped));
    const ui::Handlers handlers{desk::on_move,      desk::on_preset,      on_brightness_changed,
                                room::on_media,     room::on_setpoint,    room::on_mode,
                                room::on_lights,    room::on_light,       room::on_dial_toggle,
                                diagnostics::refresh, on_setting,         on_volume,
                                on_restart,           on_radar_page,      diagnostics::logs,
                                on_primary,           on_rail_side,       on_orientation,
                                on_screen};
    const int brightness = settings::get(settings::Key::Brightness);
    ESP_ERROR_CHECK(ui::init(handlers, brightness,
                             static_cast<std::uint32_t>(settings::get(settings::Key::Accent)),
                             settings::enabled(settings::Key::RailSide), flipped));
    on_brightness_changed(brightness);
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(desk::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("desk", 25));
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
}
