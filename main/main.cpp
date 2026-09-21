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

// The panel's own slider and Home Assistant both set brightness, so the value
// has to go back into what gets published either way.
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
                                room::on_tile,   room::on_setpoint, room::on_mode,
                                room::on_lights, room::on_light,    room::on_dial_toggle};
    ESP_ERROR_CHECK(ui::init(handlers, board::kDefaultBrightness));
    room::init();
    ESP_ERROR_CHECK(desk::start());
    ESP_ERROR_CHECK(battery::start());
    // Not fatal: no chime is better than no panel.
    ESP_ERROR_CHECK_WITHOUT_ABORT(sound::init());
    // Not fatal: a desk controller that cannot reach the network is still a
    // desk controller, and the SDIO link to the co-processor is one more thing
    // that can be unplugged.
    ESP_ERROR_CHECK_WITHOUT_ABORT(wifi::start());
    ESP_ERROR_CHECK(wallclock::start());
    ESP_ERROR_CHECK_WITHOUT_ABORT(telemetry::start());

    ESP_LOGI(TAG, "up");
    // app_main returns; the LVGL, loctek and supervisor tasks carry on.
}
