#pragma once

#include "lvgl.h"

#include <array>
#include <cstdint>

namespace ui {

/**
 * @brief Four seven-segment digits drawn from plain rectangles.
 *
 * Unlit segments are left faintly visible rather than hidden, which is what
 * makes a real segment display readable as one -- you can see the shape of the
 * digits that are not on.
 */
class SegmentDisplay {
public:
    explicit SegmentDisplay(lv_obj_t *parent);

    /** Value in tenths, so 1150 shows as 115.0. Negative shows ---.- instead. */
    void set_tenths(int tenths, std::uint32_t colour);

    lv_obj_t *object() const { return root_; }

private:
    struct Digit {
        std::array<lv_obj_t *, 7> bars{};
        // Restyling a bar invalidates it, and there are 28 of them. Remember
        // what each digit is showing so a height update only redraws the
        // segments that actually changed -- usually one or two.
        int           shown  = -2;
        std::uint32_t colour = 0;
    };

    void set_digit(int index, int value, std::uint32_t colour);

    lv_obj_t            *root_ = nullptr;
    std::array<Digit, 4> digits_{};
    lv_obj_t            *dot_ = nullptr;
};

}  // namespace ui
