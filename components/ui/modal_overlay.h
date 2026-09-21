#pragma once

#include "lvgl.h"

#include <cstdint>

namespace ui {

/**
 * @brief A dimmed overlay with a card that slides up into the page.
 *
 * Callers put their widgets in content() and drive it with open()/close().
 */
class ModalOverlay {
public:
    ModalOverlay(lv_obj_t *parent, std::int32_t width, std::int32_t height);

    lv_obj_t *content() const { return card_; }

    /** @param origin Kept for callers; the card slides rather than growing from it. */
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
    std::int32_t rest_y_  = 0;  // where the card sits once it has arrived
    bool         visible_ = false;
};

}  // namespace ui
