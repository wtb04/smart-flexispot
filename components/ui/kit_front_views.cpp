// The fullscreen views, music, cinema and the focus timer, with the focus
// timer's card from the dock; the favourites; and the notices.

#include "kit_front.h"

namespace ui::kitfront {
void build_views(lv_obj_t *) {}

void open_focus(lv_obj_t *source)
{
    lv_obj_t *body = kit::popover(source, 440, 200, kit::From::Beside);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    kit::heading(body, "Focus", "Being built");
}
}  // namespace ui::kitfront

namespace ui::detail {
bool         notice_on_show() { return false; }
std::int64_t cinema_dark_after() { return 0; }
bool         cinema_offers_skip() { return false; }
}  // namespace ui::detail
