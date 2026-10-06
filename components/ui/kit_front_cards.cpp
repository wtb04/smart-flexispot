// The bar's cards: what plays, the lights, the heating; and the Claude Code
// sessions' pill and card.

#include "kit_front.h"

namespace ui::kitfront {
namespace {
lv_obj_t *placeholder(lv_obj_t *source, const char *title)
{
    lv_obj_t *body = kit::popover(source, 440, 200, kit::From::Below);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    kit::heading(body, title, "Being built");
    return body;
}
}  // namespace

void open_media(lv_obj_t *source) { placeholder(source, "Now playing"); }
void open_lights(lv_obj_t *source) { placeholder(source, "Lights"); }
void open_heating(lv_obj_t *source) { placeholder(source, "Heating"); }
void build_claude(lv_obj_t *) {}
}  // namespace ui::kitfront
