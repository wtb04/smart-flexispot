#include "segment_display.h"

#include "theme.h"

#include <algorithm>

namespace ui {
namespace {
constexpr std::uint8_t DIGIT_MASK[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                         0x6d, 0x7d, 0x07, 0x7f, 0x6f};

constexpr std::uint8_t DASH_MASK = 0x40;
constexpr int          BLANK     = -1;

constexpr int RADIX          = 10;
constexpr int PLACE_VALUES[] = {1000, 100, 10, 1};
constexpr int MAX_TENTHS     = PLACE_VALUES[0] * RADIX - 1;

constexpr std::int32_t BAR_LONG    = 36;
constexpr std::int32_t BAR_THICK   = 8;
constexpr std::int32_t BAR_TALL    = 42;
constexpr std::int32_t BAR_RADIUS  = BAR_THICK / 2;
// The upright bars reach a pixel into the top and bottom bars.
constexpr std::int32_t BAR_OVERLAP = 1;

constexpr std::int32_t UPPER_Y  = BAR_THICK - BAR_OVERLAP;
constexpr std::int32_t MIDDLE_Y = UPPER_Y + BAR_TALL;
constexpr std::int32_t LOWER_Y  = MIDDLE_Y + BAR_THICK;
constexpr std::int32_t BOTTOM_Y = LOWER_Y + BAR_TALL - BAR_OVERLAP;
constexpr std::int32_t RIGHT_X  = BAR_LONG + BAR_THICK;

constexpr std::int32_t DIGIT_W     = BAR_LONG + 2 * BAR_THICK;
constexpr std::int32_t DIGIT_GAP   = 6;
constexpr std::int32_t DIGIT_PITCH = DIGIT_W + DIGIT_GAP;
// The last digit, the tenths, stands one gap further off to make room for the point.
constexpr std::int32_t DIGIT_X[] = {0, DIGIT_PITCH, 2 * DIGIT_PITCH, 3 * DIGIT_PITCH + DIGIT_GAP};

constexpr std::int32_t DOT_SIZE = 10;
constexpr std::int32_t DOT_X    = 166;
constexpr std::int32_t DOT_Y    = BOTTOM_Y + BAR_THICK - DOT_SIZE;

constexpr std::int32_t EDGE_MARGIN = 6;
constexpr std::int32_t DISPLAY_W   = DIGIT_X[std::size(DIGIT_X) - 1] + DIGIT_W + EDGE_MARGIN;
constexpr std::int32_t DISPLAY_H   = BOTTOM_Y + BAR_THICK + EDGE_MARGIN;

struct Bar {
    std::int32_t x;
    std::int32_t y;
    std::int32_t w;
    std::int32_t h;
};

// In the order of the masks' bits: top, upper right, lower right, bottom,
// lower left, upper left, middle.
constexpr Bar SEGMENTS[] = {
    {BAR_THICK, 0, BAR_LONG, BAR_THICK},
    {RIGHT_X, UPPER_Y, BAR_THICK, BAR_TALL},
    {RIGHT_X, LOWER_Y, BAR_THICK, BAR_TALL},
    {BAR_THICK, BOTTOM_Y, BAR_LONG, BAR_THICK},
    {0, LOWER_Y, BAR_THICK, BAR_TALL},
    {0, UPPER_Y, BAR_THICK, BAR_TALL},
    {BAR_THICK, MIDDLE_Y, BAR_LONG, BAR_THICK},
};

constexpr lv_opa_t ON_OPACITY  = LV_OPA_COVER;
constexpr lv_opa_t OFF_OPACITY = LV_OPA_10;

lv_obj_t *make_lit_box(lv_obj_t *parent, std::int32_t radius)
{
    lv_obj_t *box = lv_obj_create(parent);
    theme::fill_accent(box);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, radius, 0);
    lv_obj_set_scrollable(box, false);
    return box;
}

}  // namespace

SegmentDisplay::SegmentDisplay(lv_obj_t *parent)
{
    static_assert(std::size(SEGMENTS) == kSegmentCount);
    static_assert(std::size(DIGIT_X) == kDigitCount && std::size(PLACE_VALUES) == kDigitCount);

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, DISPLAY_W, DISPLAY_H);
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollable(root_, false);

    for (int d = 0; d < kDigitCount; ++d) {
        for (int s = 0; s < kSegmentCount; ++s) {
            const Bar &bar = SEGMENTS[s];
            lv_obj_t  *obj = make_lit_box(root_, BAR_RADIUS);
            lv_obj_set_pos(obj, DIGIT_X[d] + bar.x, bar.y);
            lv_obj_set_size(obj, bar.w, bar.h);
            digits_[d].bars[s] = obj;
        }
    }

    dot_ = make_lit_box(root_, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, 0);
    lv_obj_set_pos(dot_, DOT_X, DOT_Y);
    lv_obj_set_size(dot_, DOT_SIZE, DOT_SIZE);

    set_tenths(-1);  // dashes until there is a height, not every bar lit
}

void SegmentDisplay::set_digit(int index, int value)
{
    Digit &digit = digits_[index];
    if (digit.shown == value) {
        return;
    }
    digit.shown = value;

    const std::uint8_t mask = value == kDash                  ? DASH_MASK
                              : (value >= 0 && value < RADIX) ? DIGIT_MASK[value]
                                                              : 0;
    for (int s = 0; s < kSegmentCount; ++s) {
        lv_obj_set_style_bg_opa(digit.bars[s], (mask & (1u << s)) ? ON_OPACITY : OFF_OPACITY, 0);
    }
}

void SegmentDisplay::set_tenths(int tenths)
{
    if (tenths < 0) {
        for (int d = 0; d < kDigitCount; ++d) {
            set_digit(d, d == 0 ? BLANK : kDash);
        }
        return;
    }

    tenths = std::clamp(tenths, 0, MAX_TENTHS);
    for (int d = 0; d < kDigitCount; ++d) {
        const int  place        = PLACE_VALUES[d];
        const bool leading_zero = d == 0 && tenths < place;
        set_digit(d, leading_zero ? BLANK : (tenths / place) % RADIX);
    }
}

}  // namespace ui
