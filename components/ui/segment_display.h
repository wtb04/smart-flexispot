#pragma once

#include "lvgl.h"

#include <array>
#include <cstdint>

namespace ui {
class SegmentDisplay {
public:
    /** Height as the rail shows it, 115.0; or with clock, minutes and seconds
     *  around a colon, 24:59. scale is in percent of the rail's size. */
    explicit SegmentDisplay(lv_obj_t *parent, int scale = 100, bool clock = false);

    /** Value in tenths, so 1150 shows as 115.0. Negative shows ---.- instead. */
    void set_tenths(int tenths);

    /** For a clock: 0 to 99 minutes, and seconds. */
    void set_clock(int minutes, int seconds);

    /** For a clock: the colon lit or dark, so it can beat the seconds. */
    void set_colon(bool on);

    /** The accent, or another colour for the lit bars. */
    void set_ink(bool accent, std::uint32_t colour);

    lv_obj_t *object() const { return root_; }

private:
    struct Digit {
        std::array<lv_obj_t *, 7> bars{};
        int shown = -2;
    };

    void set_digit(int index, int value);

    lv_obj_t                 *root_ = nullptr;
    std::array<Digit, 4>      digits_{};
    std::array<lv_obj_t *, 2> dots_{};  // the decimal point, or the colon's two
    bool                      clock_ = false;
};

}  // namespace ui
