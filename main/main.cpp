#include "battery.h"
#include "ble.h"
#include "board.h"
#include "desk.h"
#include "diagnostics.h"
#include "ical.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#include "travel.h"
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

void restart_for_desk(bool bluetooth);

void on_move(ui::Move direction)
{
    desk::on_move(direction == ui::Move::Up     ? desk::Move::Up
                  : direction == ui::Move::Down ? desk::Move::Down
                                                : desk::Move::Stop);
}

void show_preset_active(int index, bool active)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_preset_active(index, active));
}

void show_height(int height_mm)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_height(height_mm));
}

void show_desk_available(bool linked)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_desk_available(linked));
}

void show_notice(const char *message, const char *level, int timeout_ms)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("", message, level, timeout_ms));
}

void on_setting(ui::Setting setting, bool on)
{
    switch (setting) {
        case ui::Setting::Charging:
            // The battery task switches it, off the screen's task: it is an I2C
            // transaction and may be refused for a pack too flat to charge.
            settings::set(settings::Key::Charging, on);
            battery::refresh();
            if (power::State state{}; on && power::last(state) && !state.present) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify(
                    "", "No battery to charge. If one is in, it is too flat: take it out and put it back",
                    "warning", 6000));
            }
            break;
        case ui::Setting::PresenceGate:
            settings::set(settings::Key::PresenceGate, on);
            break;
        case ui::Setting::DeskBluetooth:
            settings::set(settings::Key::DeskBluetooth, on);
            restart_for_desk(on);
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
    board::set_flipped(flipped);
    settings::set(settings::Key::Flipped, flipped);
}

void on_details(const char *hex, const char *callsign)
{
    radar::request_details(hex, callsign);
}

void on_journey(std::int64_t arrive_by, bool to_work)
{
    travel::want(arrive_by, to_work ? travel::Place::Work : travel::Place::Study);
}

void on_radar_page(bool showing, bool reachable)
{
    radar::set_enabled(reachable);
    radar::set_active(showing);
}

void on_calendar()
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_calendar());
}

void on_travel()
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_calendar());
}

void restart_now(void *)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_off());
    esp_restart();
}

// Wire or Bluetooth is settled at boot, when each claims the desk, so switching
// is a restart, announced so it is not mistaken for a crash.
void restart_for_desk(bool bluetooth)
{
    settings::flush();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify(
        "", bluetooth ? "Switching the desk to Bluetooth" : "Switching the desk to the wire",
        "info", 1500));
    static esp_timer_handle_t timer = nullptr;
    if (timer == nullptr) {
        const esp_timer_create_args_t args{.callback = restart_now, .name = "desk-restart"};
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&args, &timer));
    }
    if (timer != nullptr) {
        esp_timer_stop(timer);
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(timer, 1500 * 1000));
    }
}

void on_restart()
{
    ESP_LOGI(TAG, "restart requested from the panel");
    ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_off());
    esp_restart();
}
}  // namespace

// cJSON's nodes are 40 bytes, under the size that goes to internal RAM by
// default, and one Home Assistant state dump makes thousands of them: internal
// RAM is what the radio needs.
void *json_malloc(std::size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

extern "C" void app_main(void)
{
    cJSON_Hooks hooks{json_malloc, heap_caps_free};
    cJSON_InitHooks(&hooks);

    if (esp_err_t err = nvs_flash_init();
        err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_init());
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(logbuf::start(diagnostics::channels(), diagnostics::route));
    ESP_ERROR_CHECK_WITHOUT_ABORT(settings::load());

    const bool flipped = settings::enabled(settings::Key::Flipped);
    ESP_ERROR_CHECK(board::init(flipped));
    // Named, since several share a signature and a swap would still compile.
    const ui::Handlers handlers{
        .move        = on_move,
        .preset      = desk::on_preset,
        .brightness  = on_brightness_changed,
        .media       = room::on_media,
        .setpoint    = room::on_setpoint,
        .mode        = room::on_mode,
        .lights      = room::on_lights,
        .light       = room::on_light,
        .dial_toggle = room::on_dial_toggle,
        .diagnostics = diagnostics::refresh,
        .setting     = on_setting,
        .volume      = on_volume,
        .restart     = on_restart,
        .radar       = on_radar_page,
        .log         = diagnostics::logs,
        .primary     = on_primary,
        .rail_side   = on_rail_side,
        .orientation = on_orientation,
        .screen      = on_screen,
        .details     = on_details,
        .journey     = on_journey,
    };
    const int brightness = settings::get(settings::Key::Brightness);
    ui::set_cards(diagnostics::cards(), diagnostics::card_count());
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
    const desk::View desk_view{
        .preset_active = show_preset_active,
        .height        = show_height,
        .available     = show_desk_available,
        .notice        = show_notice,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(desk::start(settings::enabled(settings::Key::DeskBluetooth)
                                                  ? desk::Link::Bluetooth
                                                  : desk::Link::Wire,
                                              desk_view));
    ESP_ERROR_CHECK_WITHOUT_ABORT(rtc::start());

    ESP_ERROR_CHECK_WITHOUT_ABORT(battery::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(sound::init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("desk"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi::start());
    ESP_ERROR_CHECK(wallclock::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("network"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(telemetry::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(diagnostics::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ical::start(on_calendar));
    ESP_ERROR_CHECK_WITHOUT_ABORT(travel::start(on_travel));

    ESP_LOGI(TAG, "up");
}
