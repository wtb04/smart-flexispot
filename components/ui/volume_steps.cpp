#include "volume_steps.h"

#include "media_model.h"
#include "theme.h"

namespace ui::detail {
namespace {
constexpr lv_opa_t PRESSED_OPA = LV_OPA_20;
constexpr int      DIVIDER_W   = 2;

lv_obj_t *half(lv_obj_t *bar, lv_align_t side, const char *symbol, const lv_font_t *font, MediaAction action)
{
    lv_obj_t *button = lv_obj_create(bar);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, lv_pct(50), lv_pct(100));
    lv_obj_align(button, side, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme::text), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, PRESSED_OPA, LV_STATE_PRESSED);
    lv_obj_set_gesture_bubble(button, false);
    lv_obj_add_event_cb(
        button, [](lv_event_t *e) { media_action(static_cast<MediaAction>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)))); },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
    lv_obj_t *label = theme::make_label(button, symbol, theme::text, font);
    lv_obj_center(label);
    lv_obj_set_hidden(button, true);
    return button;
}
}  // namespace

VolumeSteps make_volume_steps(lv_obj_t *bar, lv_obj_t *speaker, lv_obj_t *level, lv_obj_t *fill,
                              const lv_font_t *font)
{
    VolumeSteps steps{speaker, level, fill};
    steps.down    = half(bar, LV_ALIGN_LEFT_MID, LV_SYMBOL_MINUS, font, MediaAction::VolumeDown);
    steps.up      = half(bar, LV_ALIGN_RIGHT_MID, LV_SYMBOL_PLUS, font, MediaAction::VolumeUp);
    steps.divider = lv_obj_create(bar);
    lv_obj_remove_style_all(steps.divider);
    lv_obj_set_size(steps.divider, DIVIDER_W, lv_pct(50));
    lv_obj_center(steps.divider);
    lv_obj_set_style_bg_color(steps.divider, lv_color_hex(theme::text), 0);
    lv_obj_set_style_bg_opa(steps.divider, LV_OPA_30, 0);
    lv_obj_set_clickable(steps.divider, false);
    lv_obj_set_hidden(steps.divider, true);
    return steps;
}

void show_volume_steps(const VolumeSteps &steps, bool stepping)
{
    for (lv_obj_t *part : {steps.down, steps.up, steps.divider}) {
        lv_obj_set_hidden(part, !stepping);
    }
    for (lv_obj_t *part : {steps.speaker, steps.level, steps.fill}) {
        lv_obj_set_hidden(part, stepping);
    }
}

bool step_volume(lv_obj_t *, lv_event_t *)
{
    return media_state().steps_volume;
}
}  // namespace ui::detail
