#include "battery.h"
#include "board.h"
#include "desk.h"
#include "esp_log.h"
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
}
}  // namespace

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(board::init());
    const ui::Handlers handlers{desk::on_move,   desk::on_preset,   on_brightness_changed,
                                room::on_media,  room::on_setpoint, room::on_mode,
                                room::on_lights, room::on_light,    room::on_dial_toggle};
    ESP_ERROR_CHECK(ui::init(handlers, board::kDefaultBrightness));
    room::init();
    // Neither is fatal: panicking here put the panel in a boot loop over a
    // peripheral it can perfectly well run without.
    ESP_ERROR_CHECK_WITHOUT_ABORT(desk::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("desk", 25));
    ESP_ERROR_CHECK_WITHOUT_ABORT(battery::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(sound::init());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("power", 45));
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi::start());
    ESP_ERROR_CHECK(wallclock::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("network", 70));
    ESP_ERROR_CHECK_WITHOUT_ABORT(telemetry::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::splash_step("home assistant", 85));

    ESP_LOGI(TAG, "up");
    // app_main returns; the LVGL, loctek and supervisor tasks carry on.
}
