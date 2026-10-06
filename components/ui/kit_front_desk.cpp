// The desk, folded out of the dock's height: its height over its travel,
// every preset, held to store, and up and down for as long as they are held.

#include "kit_front.h"

namespace ui::kitfront {
void open_desk(lv_obj_t *source)
{
    lv_obj_t *body = kit::popover(source, 420, 200, kit::From::Beside);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    kit::heading(body, "Desk", "Being built");
}
}  // namespace ui::kitfront
