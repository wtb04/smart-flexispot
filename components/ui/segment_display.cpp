#include "segment_display.h"

#include "theme.h"

#include <algorithm>

namespace ui {
namespace {

// Bit per segment, a through g.
constexpr std::uint8_t DIGIT_MASK[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                         0x6d, 0x7d, 0x07, 0x7f, 0x6f};

// The middle bar alone.
constexpr std::uint8_t DASH_MASK = 0x40;
constexpr int          DASH      = -2;

constexpr int DIGIT_X[4] = {0, 58, 116, 180};
constexpr int BAR_LONG   = 36;
constexpr int BAR_THICK  = 8;
constexpr int BAR_TALL   = 42;

constexpr lv_opa_t OFF_OPACITY = LV_OPA_10;

}  // namespace

SegmentDisplay::SegmentDisplay(lv_obj_t *parent)
{
    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, 238, 112);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollable(root_, false);

    for (int d = 0; d < 4; ++d) {
        for (lv_obj_t *&bar : digits_[d].bars) {
            bar = lv_obj_create(root_);
            theme::fill_accent(bar);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_radius(bar, 4, 0);
            lv_obj_set_scrollable(bar, false);
        }
        const int x = DIGIT_X[d];
        // a, b, c, d, e, f, g -- clockwise from the top, then the middle bar.
        lv_obj_set_pos(digits_[d].bars[0], x + BAR_THICK, 0);
        lv_obj_set_size(digits_[d].bars[0], BAR_LONG, BAR_THICK);
        lv_obj_set_pos(digits_[d].bars[1], x + BAR_LONG + BAR_THICK, 7);
        lv_obj_set_size(digits_[d].bars[1], BAR_THICK, BAR_TALL);
        lv_obj_set_pos(digits_[d].bars[2], x + BAR_LONG + BAR_THICK, 57);
        lv_obj_set_size(digits_[d].bars[2], BAR_THICK, BAR_TALL);
        lv_obj_set_pos(digits_[d].bars[3], x + BAR_THICK, 98);
        lv_obj_set_size(digits_[d].bars[3], BAR_LONG, BAR_THICK);
        lv_obj_set_pos(digits_[d].bars[4], x, 57);
        lv_obj_set_size(digits_[d].bars[4], BAR_THICK, BAR_TALL);
        lv_obj_set_pos(digits_[d].bars[5], x, 7);
        lv_obj_set_size(digits_[d].bars[5], BAR_THICK, BAR_TALL);
        lv_obj_set_pos(digits_[d].bars[6], x + BAR_THICK, 49);
        lv_obj_set_size(digits_[d].bars[6], BAR_LONG, BAR_THICK);
    }

    dot_ = lv_obj_create(root_);
    theme::fill_accent(dot_);
    lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot_, 166, 96);
    lv_obj_set_size(dot_, 10, 10);
    lv_obj_set_style_border_width(dot_, 0, 0);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_scrollable(dot_, false);
}

void SegmentDisplay::set_digit(int index, int value)
{
    Digit &digit = digits_[index];
    if (digit.shown == value) {
        return;
    }
    digit.shown = value;

    const std::uint8_t mask = value == DASH             ? DASH_MASK
                              : (value >= 0 && value <= 9) ? DIGIT_MASK[value]
                                                           : 0;
    for (int s = 0; s < 7; ++s) {
        lv_obj_set_style_bg_opa(digit.bars[s],
                                (mask & (1u << s)) ? static_cast<lv_opa_t>(LV_OPA_COVER) : OFF_OPACITY, 0);
    }
}

void SegmentDisplay::set_tenths(int tenths)
{
    if (tenths < 0) {
        // ---.- reads as "no reading"; a blank panel reads as "switched off".
        for (int d = 0; d < 4; ++d) {
            set_digit(d, d == 0 ? -1 : DASH);
        }
        return;
    }

    tenths = std::clamp(tenths, 0, 9999);
    set_digit(0, tenths >= 1000 ? (tenths / 1000) % 10 : -1);
    set_digit(1, (tenths / 100) % 10);
    set_digit(2, (tenths / 10) % 10);
    set_digit(3, tenths % 10);
}

}  // namespace ui
