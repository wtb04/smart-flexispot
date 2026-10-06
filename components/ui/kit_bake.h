#pragma once

#include "lvgl.h"

// Shadows worked out once rather than on every frame. A tile's is painted
// into the ground while the tile stands still; one that moves, fades or hides
// gets its live shadow back in the same frame, and is painted in again once
// it has stood still for a moment. A pane of glass keeps a picture of its
// shadow while its shape holds, and lays that down wherever it goes.
namespace ui::kit::bake {

/** The ground's picture, drawn as the background of `ground`, with `content` the plane above it. */
void begin(lv_obj_t *ground, lv_obj_t *content, lv_draw_buf_t *picture);
/** The picture was painted afresh: every painted shadow is gone from it. */
void painted();
/** The look changed: shadows go back to being live, to be painted again as they now look. */
void restyle();
/** A tile whose shadow may be painted in. */
void cast(lv_obj_t *tile);
/** A pane of glass whose shadow, while its shape holds, is a picture drawn once. `base` is its kit style. */
void glass(lv_obj_t *obj, const lv_style_t *base);

}  // namespace ui::kit::bake
