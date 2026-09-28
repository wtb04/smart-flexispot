#include "room_layout.h"

namespace room {
void show_unknown()
{
    for (int i = 0; i < PILL_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pill(i, PILLS[i].label, "--", ui::Level::Neutral));
    }
    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_dial_toggle(i, toggle_label(TOGGLES[i], false), false));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_lights("LIGHTS", "--", false));
    for (int i = 0; i < LIGHT_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, LIGHTS[i].name, "--", false));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("SPEAKER", "", "", "--", false, false));
}
}  // namespace room
