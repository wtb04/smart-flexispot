#pragma once

// What the fullscreen views share among their own files: the chrome over each
// and the bits of text they set.

#include "kit_front.h"
#include "views.h"

namespace ui::kitfront::views {

/** A fullscreen view's root on the Chrome layer, over the dock and the bar: the ground, or black in either look. */
lv_obj_t *fullscreen_root(bool black = false);
/** The dark look's ink for what sits on black, whatever the look. */
inline constexpr std::uint32_t ON_BLACK = 0xf6efe7;
/** A label on black, at `ink`'s strength. */
void ink_on_black(lv_obj_t *label, kit::Ink ink);
/** A fill button on black: faint and white in either look, lit in white. */
void on_black(lv_obj_t *button);

/** Over a view, as round chips with a light fill: Stand or Sit at the left,
 *  then its quick actions; the battery while unplugged, the time and the way
 *  back at the right. They fade once the screen goes untouched. */
struct Chrome {
    lv_obj_t *left = nullptr;  // the row the quick actions go in
};
Chrome chrome(detail::ViewId view, lv_obj_t *root, bool black = false);
/** A round button with a light fill and a filled glyph `px` big. */
lv_obj_t *fill_button(lv_obj_t *parent, const char *glyph, std::int32_t side, std::int32_t px);
/** The one primary action: a solid accent capsule, its glyph in the ink that goes on it. */
lv_obj_t *primary(lv_obj_t *parent, const char *glyph, std::int32_t w, std::int32_t h);
/** A quick action, a chip in the row at the left: lit while what it does is on. */
lv_obj_t *quick_action(const Chrome &chrome, const char *glyph);
/** A chip of two glyphs, one over the other: the desk and where it goes. */
lv_obj_t *desk_chip(lv_obj_t *parent, const char *top, const char *bottom);
/** Lit, a chip fills solid in the ink, as Control Center's do, leaving the accent to the primary action. */
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
