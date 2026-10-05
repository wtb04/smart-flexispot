#include "volume_steps.h"

#include "media_model.h"
#include "theme.h"

namespace ui::detail {
VolumeSteps make_volume_steps(lv_obj_t *bar, lv_obj_t *speaker, lv_obj_t *level, lv_obj_t *fill,
                              const lv_font_t *font, std::int32_t inset)
{
    VolumeSteps steps{speaker, level, fill, nullptr, nullptr, inset};
    steps.minus = theme::make_label(bar, LV_SYMBOL_MINUS, theme::text, font);
    lv_obj_align(steps.minus, LV_ALIGN_LEFT_MID, inset, 0);
    steps.plus = theme::make_label(bar, LV_SYMBOL_PLUS, theme::text, font);
    lv_obj_align(steps.plus, LV_ALIGN_RIGHT_MID, -inset, 0);
    for (lv_obj_t *label : {steps.minus, steps.plus}) {
        lv_obj_set_clickable(label, false);
        lv_obj_set_hidden(label, true);
    }
    return steps;
}

void show_volume_steps(const VolumeSteps &steps, bool stepping)
{
    lv_obj_set_hidden(steps.minus, !stepping);
    lv_obj_set_hidden(steps.plus, !stepping);
    lv_obj_set_hidden(steps.level, stepping);
    lv_obj_set_hidden(steps.fill, stepping);
    lv_obj_align(steps.speaker, stepping ? LV_ALIGN_CENTER : LV_ALIGN_LEFT_MID, stepping ? 0 : steps.inset, 0);
}

bool step_volume(lv_obj_t *bar, lv_event_t *e)
{
    if (!media_state().steps_volume) {
        return false;
    }
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        lv_point_t at{};
        lv_indev_get_point(lv_indev_active(), &at);
        lv_area_t area{};
        lv_obj_get_coords(bar, &area);
        media_action(at.x < area.x1 + lv_area_get_width(&area) / 2 ? MediaAction::VolumeDown : MediaAction::VolumeUp);
    }
    return true;
}
}  // namespace ui::detail
