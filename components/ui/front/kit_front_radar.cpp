// The radar page, on a page of the design kit's; kit_front_radar_view.cpp draws it.

#include "kit_front.h"

#include "radar_page.h"

namespace ui::kitfront {
void build_radar(lv_obj_t *page)
{
    build_radar_page(page, kit::inset::width, kit::inset::height);
    subscribe(Topic::Page, detail::kNoView, [] { radar_page_opened(); });
}

void radar_opens_on(const char *hex)
{
    radar_open_on(hex);
}
}  // namespace ui::kitfront
