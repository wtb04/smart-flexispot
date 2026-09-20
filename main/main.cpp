#include "board.h"
#include "desk.h"
#include "esp_log.h"
#include "ui.h"

namespace {
constexpr char TAG[] = "tab5";
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(board::init());
    ESP_ERROR_CHECK(ui::init(desk::on_move, desk::on_preset));
    ESP_ERROR_CHECK(desk::start());

    ESP_LOGI(TAG, "up");
    // app_main returns; the LVGL, loctek and supervisor tasks carry on.
}
