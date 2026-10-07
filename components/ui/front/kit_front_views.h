#pragma once

// What the fullscreen views share among their own files: the chrome over each
// and the bits of text they set.

#include "kit_front.h"
#include "views.h"

namespace ui::kitfront::views {

/** A fullscreen view's root on the Chrome layer, over the dock and the bar. */
lv_obj_t *fullscreen_root();

/** Over a view: Stand or Sit at the left, then its quick actions; the focus
 *  timer's badge, the battery while unplugged, the time and the way back at the
 *  right. All but the badge fade once the screen goes untouched. */
struct Chrome {
    lv_obj_t *left = nullptr;  // the row the quick actions go in
};
Chrome chrome(detail::ViewId view, lv_obj_t *root, bool focus_badge);
/** A quick action in the row at the left: lit while what it does is on. */
lv_obj_t *quick_action(const Chrome &chrome, const char *glyph, const char *label);
void      light(lv_obj_t *button, bool on);

void set_text_if(lv_obj_t *label, const char *value);
/** Minutes and seconds, as a track's progress is told. */
void write_clock(lv_obj_t *label, int seconds);
/** A label as wide as the widest of its figures, set right. */
lv_obj_t *figures(lv_obj_t *parent, const char *widest, lv_text_align_t align);

void open_focus_view();
void build_media(lv_obj_t *screen);   // kit_front_views_media.cpp
void build_focus(lv_obj_t *screen);   // kit_front_views_focus.cpp
void open_music();
void open_cinema();

}  // namespace ui::kitfront::views
