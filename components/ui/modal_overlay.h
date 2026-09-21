#pragma once

#include "lvgl.h"

#include <cstdint>

namespace ui {

class ModalOverlay {
public:
    ModalOverlay(lv_obj_t *parent, std::int32_t width, std::int32_t height);

    lv_obj_t *content() const { return card_; }

    /** origin is ignored: the card slides rather than growing out of it. */
    void open(lv_obj_t *origin = nullptr);
    void close();
    bool visible() const { return visible_; }

private:
    static void slide(void *target, std::int32_t value);
    static void hide_when_done(lv_anim_t *anim);
    static void scrim_clicked(lv_event_t *event);

    void start(std::int32_t from, std::int32_t to, std::uint32_t duration, bool hide_at_end);

    lv_obj_t    *scrim_ = nullptr;
    lv_obj_t    *card_  = nullptr;
    std::int32_t width_;
    std::int32_t height_;
    std::int32_t rest_y_  = 0;
    bool         visible_ = false;
};

}  // namespace ui
