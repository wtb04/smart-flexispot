#pragma once

#include "lvgl.h"

#include <cstdint>

// A card that unfolds from a corner over the screen, as the desk and the focus
// timer fold out of the dock and what plays, the heating and the lights drop
// from the control bar. It stays where it ends
// up while what shows of it grows across and down, so each frame draws only
// what is newly shown or hidden; sliding a card drew all of it, and what it
// passed over, on every frame. A tap anywhere else folds it away, and so does
// being left alone. One is out at a time. On the LVGL task only.
namespace ui::detail {

struct Popout {
    lv_obj_t    *scrim    = nullptr;  // clear, under the card: a tap on it folds the card away
    lv_obj_t    *frame    = nullptr;  // what shows of the card
    lv_obj_t    *ring     = nullptr;  // background round the card, setting it apart from the page
    lv_obj_t    *card     = nullptr;
    lv_obj_t    *button   = nullptr;  // what opens it, lit while it is out
    lv_obj_t    *above    = nullptr;  // kept over the scrim while it is out, to be used meanwhile
    void (*lit)(bool open) = nullptr;  // told as it comes and goes, to paint its button
    lv_timer_t  *idle     = nullptr;
    std::uint32_t idle_ms = 0;      // left untouched this long, it folds away
    std::int32_t x        = 0;  // the corner it unfolds from
    std::int32_t y        = 0;
    bool         leftward = false;  // across to the left of the corner, else to the right
    bool         upward   = false;  // up from the corner, else down
    std::int32_t near     = 0;      // from the corner to the card, across
    std::int32_t top      = 0;      // and down, or up
    std::int32_t full_w   = 0;      // all of it shown, the ring of background round the card too
    std::int32_t full_h   = 0;
    int          shown    = 0;      // in thousandths
    std::int32_t shown_w  = 0;
    std::int32_t shown_h  = 0;
    bool         open     = false;
};

/** The scrim, the frame and a card `w` wide on `parent`, the card returned to
 *  be filled. It folds away by itself after `idle_ms` without a press on it. */
lv_obj_t *build_popout(Popout &p, lv_obj_t *parent, std::int32_t w, std::int32_t near, std::int32_t top,
                       std::uint32_t idle_ms);

/** Where it unfolds from; drawn again whole if it is out. */
void place_popout(Popout &p, std::int32_t x, std::int32_t y, bool leftward, bool upward = false);

/** After the card's height changes. */
void fit_popout(Popout &p);

void open_popout(Popout &p, bool open);

/** Whichever is out, as the page changes under it. */
void close_popout();

/** Whether it is still unfolding or folding away. */
bool popout_moving(const Popout &p);

}  // namespace ui::detail
