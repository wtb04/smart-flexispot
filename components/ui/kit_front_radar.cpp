// The Radar page.

#include "kit_front.h"

namespace ui::kitfront {
void build_radar(lv_obj_t *page)
{
    lv_obj_t *t = kit::tile(page, kit::Lift::Low);
    lv_obj_set_size(t, kit::inset::width, 200);
    kit::text(t, kit::Role::H2, "Radar");
    lv_obj_align(kit::text(t, kit::Role::Body, "Being built", kit::Ink::Secondary), LV_ALIGN_BOTTOM_LEFT, 0, 0);
}
}  // namespace ui::kitfront
