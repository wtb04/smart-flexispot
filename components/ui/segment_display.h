#pragma once

#include "lvgl.h"

#include <array>
#include <cstdint>

namespace ui {
class SegmentDisplay {
public:
    explicit SegmentDisplay(lv_obj_t *parent);

    /** Value in tenths, so 1150 shows as 115.0. Negative shows ---.- instead. */
    void set_tenths(int tenths);

    lv_obj_t *object() const { return root_; }

private:
    struct Digit {
        std::array<lv_obj_t *, 7> bars{};
        int shown = -2;
    };

    void set_digit(int index, int value);

    lv_obj_t            *root_ = nullptr;
    std::array<Digit, 4> digits_{};
    lv_obj_t            *dot_ = nullptr;
};

}  // namespace ui
