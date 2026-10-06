// The radar page: the old screen's map, planes and zoom, which it keeps, on a
// page of the design kit's.

#include "kit_front.h"

#include "radar_page.h"

namespace ui::kitfront {
void build_radar(lv_obj_t *page)
{
    build_radar_page(page, kit::inset::width, kit::inset::height);
    subscribe(Topic::Page, detail::kNoView, [] { radar_page_opened(); });
}
}  // namespace ui::kitfront
