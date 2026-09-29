#include "battery.h"
#include "ble.h"
#include "ble_desk.h"
#include "board.h"
#include "desk.h"
#include "diagnostics.h"
#include "focus.h"
#include "ical.h"
#include "imu.h"
#include "orientation.h"
#include "remote.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "last_words.h"
#include "logbuf.h"
#include "ota.h"
#include "nvs_flash.h"
#include "power.h"
#include "backup_clock.h"
#include "radar.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include "sound.h"
#include "room.h"
#include "network.h"
#include "travel.h"
#include "ui.h"
#include "wallclock.h"
#include "wifi.h"

#include "units.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
constexpr char TAG[] = "panel";

constexpr int         FOCUS_NOTICE_MS    = 5000;
constexpr std::size_t FOCUS_MESSAGE_SIZE = 64;
constexpr std::time_t WALL_CLOCK_SET     = 1'700'000'000;  // any earlier and the clock is not set yet

// The notice stays up until the restart takes the screen down.
constexpr int DESK_RESTART_DELAY_MS = 1500;

// heap_caps_malloc_prefer takes the number of capability sets that follow it.
constexpr std::size_t JSON_HEAP_CHOICES = 2;

void on_brightness_changed(int percent)
{
    board::set_brightness_percent(percent);
    network::note_brightness(percent);
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

void show_notice(const char *message, desk::Tone tone, int timeout_ms)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify(
        "Desk", "", message, tone == desk::Tone::Done ? ui::Level::Good : ui::Level::Neutral, timeout_ms));
}

void on_setting(ui::Setting setting, bool on)
{
    switch (setting) {
        case ui::Setting::Charging:
            // The battery task switches it, off the screen's task: it is an I2C
            // transaction and may be refused for a pack too flat to charge.
            settings::set(settings::Key::Charging, on);
            battery::refresh();
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
    network::note_screen(on);
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

void on_orientation(ui::Orientation orientation)
{
    const bool automatic = orientation == ui::Orientation::Auto;
    settings::set(settings::Key::OrientAuto, automatic);
    if (automatic) {
        orientation::calibrate_and_refresh();
        return;
    }
    const bool flipped = orientation == ui::Orientation::Flipped;
    board::set_flipped(flipped);
    settings::set(settings::Key::Flipped, flipped);
}

ui::Focus focus_view(const focus::State &state)
{
    const focus::Plan plan = focus::plan();
    return ui::Focus{
        .phase          = static_cast<ui::FocusPhase>(state.phase),
        .round          = state.round,
        .rounds         = plan.rounds,
        .running        = state.running,
        .ends_at_ms     = state.ends_at,
        .left_ms        = state.left,
        .length_ms      = state.length,
        .work_min       = plan.work_min,
        .break_min      = plan.break_min,
        .long_break_min = plan.long_break_min,
    };
}

focus::Plan stored_plan();

// The wall clock in seconds, or 0 before it is set.
std::int64_t wall_seconds()
{
    const std::time_t now = std::time(nullptr);
    return now >= WALL_CLOCK_SET ? static_cast<std::int64_t>(now) : 0;
}

void save_focus(const focus::State &state)
{
    const focus::Saved kept = focus::saved(state, focus::now_ms(), wall_seconds());
    settings::set(settings::Key::FocusPhase, static_cast<int>(kept.phase));
    settings::set(settings::Key::FocusRound, kept.round);
    settings::set(settings::Key::FocusRunning, kept.running ? 1 : 0);
    settings::set(settings::Key::FocusLeft, kept.left);
    settings::set(settings::Key::FocusLength, kept.length);
    settings::set(settings::Key::FocusEnds, static_cast<int>(kept.ends_s));
}

focus::State stored_focus()
{
    focus::Saved kept;
    kept.phase   = static_cast<focus::Phase>(settings::get(settings::Key::FocusPhase));
    kept.round   = settings::get(settings::Key::FocusRound);
    kept.running = settings::enabled(settings::Key::FocusRunning);
    kept.left    = settings::get(settings::Key::FocusLeft);
    kept.length  = settings::get(settings::Key::FocusLength);
    kept.ends_s  = settings::get(settings::Key::FocusEnds);
    return focus::restored(kept, stored_plan(), focus::now_ms(), wall_seconds());
}

// A part running out chimes and says what comes next, on whatever page is up.
void on_focus_change(const focus::State &state, bool finished)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_focus(focus_view(state)));
    save_focus(state);
    if (!finished) {
        return;
    }
    const focus::Plan plan = focus::plan();
    char              title[FOCUS_MESSAGE_SIZE];
    char              message[FOCUS_MESSAGE_SIZE];
    switch (state.phase) {
        case focus::Phase::Break:
            std::snprintf(title, sizeof(title), "Round %d done", state.round);
            std::snprintf(message, sizeof(message), "A %d min break is ready", plan.break_min);
            break;
        case focus::Phase::LongBreak:
            std::snprintf(title, sizeof(title), "All %d rounds done", plan.rounds);
            std::snprintf(message, sizeof(message), "A %d min break is ready", plan.long_break_min);
            break;
        case focus::Phase::Work:
            std::snprintf(title, sizeof(title), "Break over");
            std::snprintf(message, sizeof(message), "Round %d of %d is ready", state.round, plan.rounds);
            break;
        case focus::Phase::Idle:
            return;
    }
    sound::ding();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("Focus", title, message, ui::Level::Good, FOCUS_NOTICE_MS));
}

focus::Plan stored_plan()
{
    focus::Plan plan;
    plan.work_min       = settings::get(settings::Key::FocusWork);
    plan.break_min      = settings::get(settings::Key::FocusBreak);
    plan.long_break_min = settings::get(settings::Key::FocusLong);
    plan.rounds         = settings::get(settings::Key::FocusRounds);
    return plan;
}

void on_focus_plan(int work_min, int break_min, int long_break_min, int rounds)
{
    settings::set(settings::Key::FocusWork, work_min);
    settings::set(settings::Key::FocusBreak, break_min);
    settings::set(settings::Key::FocusLong, long_break_min);
    settings::set(settings::Key::FocusRounds, rounds);
    focus::set_plan(stored_plan());  // settings has clamped them
}

void on_focus(ui::FocusAction action)
{
    focus::act(action == ui::FocusAction::Toggle ? focus::Action::Toggle
               : action == ui::FocusAction::Skip ? focus::Action::Skip
                                                 : focus::Action::Reset);
}

void on_details(const char *hex, const char *callsign)
{
    radar::request_details(hex, callsign);
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
    settings::flush();  // a flash write: here on the timer task, not on the screen's
    ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_off());
    esp_restart();
}

// Wire or Bluetooth is settled at boot, when each claims the desk, so switching
// is a restart, announced so it is not mistaken for a crash.
void restart_for_desk(bool bluetooth)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify(
        "Desk", "", bluetooth ? "Switching the desk to Bluetooth" : "Switching the desk to the wire",
        ui::Level::Neutral, DESK_RESTART_DELAY_MS));
    static esp_timer_handle_t timer = nullptr;
    if (timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = restart_now;
        args.name     = "desk-restart";
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&args, &timer));
    }
    if (timer != nullptr) {
        esp_timer_stop(timer);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_start_once(timer, DESK_RESTART_DELAY_MS * units::kUsPerMs));
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
    return heap_caps_malloc_prefer(size, JSON_HEAP_CHOICES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

namespace {
// What the update server needs from the rest of the panel.
bool desk_moving()
{
    return std::strcmp(desk::motion(), "idle") != 0;
}

void show_update(const ota::Status &status)
{
    const ui::UpdateTarget busy = status.busy == ota::Target::Panel       ? ui::UpdateTarget::Panel
                                  : status.busy == ota::Target::Companion ? ui::UpdateTarget::Companion
                                                                          : ui::UpdateTarget::None;
    const ui::UpdatePhase phase = status.phase == ota::Phase::Installing ? ui::UpdatePhase::Installing
                                                                         : ui::UpdatePhase::Receiving;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_update({.busy = busy, .phase = phase, .percent = status.percent,
                        .seconds_left = status.seconds_left, .immediate = status.immediate,
                        .panel_ready = status.panel_ready,
                        .companion_ready = status.companion_ready}));
}

void before_update_restart()
{
    settings::flush();
    ESP_ERROR_CHECK_WITHOUT_ABORT(board::display_off());
}
}  // namespace

extern "C" void app_main(void)
{
    board::dark_from_the_start();
    cJSON_Hooks hooks{json_malloc, heap_caps_free};
    cJSON_InitHooks(&hooks);
    ota::watch();

    if (esp_err_t err = nvs_flash_init();
        err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_init());
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(logbuf::start(diagnostics::channels(), diagnostics::route));
    last_words::start();
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
        .focus       = on_focus,
        .focus_plan  = on_focus_plan,
        .update_now  = ota::install,
        .pick        = room::on_pick,
        .seek        = room::on_seek,
        .media_volume = room::on_media_volume,
    };
    const int brightness = settings::get(settings::Key::Brightness);
    ui::set_cards(diagnostics::cards(), diagnostics::card_count());
    ESP_ERROR_CHECK(ui::init(handlers, brightness,
                             static_cast<std::uint32_t>(settings::get(settings::Key::Accent)),
                             settings::enabled(settings::Key::RailSide),
                             settings::enabled(settings::Key::OrientAuto) ? ui::Orientation::Auto
                             : flipped                                    ? ui::Orientation::Flipped
                                                                          : ui::Orientation::Normal));
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(imu::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(orientation::start());

    ESP_ERROR_CHECK_WITHOUT_ABORT(battery::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(sound::init());
    // Read before anything changes the timer, which saves over what is stored.
    const focus::State kept_focus = stored_focus();
    ESP_ERROR_CHECK_WITHOUT_ABORT(focus::start(on_focus_change));
    focus::set_plan(stored_plan());  // which shows it too
    focus::restore(kept_focus);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("desk"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ota::start({
        .busy    = desk_moving,
        .status  = show_update,
        .restart = before_update_restart,
        .relay   = ble::desk::send_update,
    }));
    ESP_ERROR_CHECK_WITHOUT_ABORT(remote::start());
    ESP_ERROR_CHECK(wallclock::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("network"));
    ESP_ERROR_CHECK_WITHOUT_ABORT(network::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(diagnostics::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ical::start(on_calendar));
    ESP_ERROR_CHECK_WITHOUT_ABORT(travel::start(on_travel));

    ESP_LOGI(TAG, "up");
}
