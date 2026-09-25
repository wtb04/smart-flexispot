#pragma once

#include "lvgl.h"

#include <cstdint>

namespace ui {
class ModalOverlay {
public:
    ModalOverlay(lv_obj_t *parent, std::int32_t width, std::int32_t height);

    lv_obj_t *content() const { return card_; }

    /** Puts a close button in the card's top right corner. Worth having on any
     *  card large enough that the scrim around it is not a target. */
    void add_close_button();

    /** Height of the band add_close_button() occupies, for laying out below it. */
    static std::int32_t header_height();

    /** For a card whose contents differ each time it is opened. Call before open(). */
    void resize(std::int32_t width, std::int32_t height);

    /** origin is ignored: a card small enough slides in, a larger one appears. */
    void open(lv_obj_t *origin = nullptr);
    void close();
    bool visible() const { return visible_; }

private:
    static void slide(void *target, std::int32_t value);
    static void hide_when_done(lv_anim_t *anim);
    static void scrim_clicked(lv_event_t *event);
    static void close_clicked(lv_event_t *event);

    void start(std::int32_t from, std::int32_t to, std::uint32_t duration, bool hide_at_end);
    bool slides() const;

    lv_obj_t    *scrim_ = nullptr;
    lv_obj_t    *card_  = nullptr;
    std::int32_t width_;
    std::int32_t height_;
    std::int32_t rest_y_  = 0;
    bool         visible_ = false;
};

}  // namespace ui
