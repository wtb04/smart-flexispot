#include "segment_display.h"

#include "theme.h"

#include <algorithm>

namespace ui {
namespace {
constexpr std::uint8_t DIGIT_MASK[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66,
                                         0x6d, 0x7d, 0x07, 0x7f, 0x6f};

constexpr std::uint8_t DASH_MASK = 0x40;
constexpr int          DASH      = -2;

constexpr int BAR_LONG  = 36;
constexpr int BAR_THICK = 8;
constexpr int BAR_TALL  = 42;
constexpr int PITCH     = 58;  // one digit to the next
constexpr int POINT     = 10;

constexpr lv_opa_t OFF_OPACITY = LV_OPA_10;

lv_obj_t *bar_of(lv_obj_t *parent, int radius)
{
    lv_obj_t *bar = lv_obj_create(parent);
    theme::fill_accent(bar);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, radius, 0);
    lv_obj_set_scrollable(bar, false);
    lv_obj_set_clickable(bar, false);
    return bar;
}

}  // namespace

// Drawn at the rail's size and scaled as it is placed, so every size has the
// same proportions.
SegmentDisplay::SegmentDisplay(lv_obj_t *parent, int scale, bool clock) : clock_(clock)
{
    const auto at = [scale](int value) { return value * scale / 100; };
    // A clock leaves room for its colon between the pairs; a height leaves
    // room for its point before the last digit.
    const int digit_x[4] = {0, PITCH, clock ? 2 * PITCH + 18 : 2 * PITCH,
                            clock ? 3 * PITCH + 18 : 3 * PITCH + 6};

    root_ = lv_obj_create(parent);
    lv_obj_set_size(root_, at(digit_x[3] + BAR_LONG + 2 * BAR_THICK + 6), at(112));
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_scrollable(root_, false);
    lv_obj_set_clickable(root_, false);

    const int radius = at(4);
    for (int d = 0; d < 4; ++d) {
        for (lv_obj_t *&bar : digits_[d].bars) {
            bar = bar_of(root_, radius);
        }
        const int x    = digit_x[d];
        auto      bars = digits_[d].bars;
        const auto place = [&](int i, int bx, int by, int w, int h) {
            lv_obj_set_pos(bars[i], at(bx), at(by));
            lv_obj_set_size(bars[i], std::max(1, at(w)), std::max(1, at(h)));
        };
        place(0, x + BAR_THICK, 0, BAR_LONG, BAR_THICK);
        place(1, x + BAR_LONG + BAR_THICK, 7, BAR_THICK, BAR_TALL);
        place(2, x + BAR_LONG + BAR_THICK, 57, BAR_THICK, BAR_TALL);
        place(3, x + BAR_THICK, 98, BAR_LONG, BAR_THICK);
        place(4, x, 57, BAR_THICK, BAR_TALL);
        place(5, x, 7, BAR_THICK, BAR_TALL);
        place(6, x + BAR_THICK, 49, BAR_LONG, BAR_THICK);
    }

    const int dot_count = clock ? 2 : 1;
    for (int i = 0; i < dot_count; ++i) {
        dots_[i] = bar_of(root_, LV_RADIUS_CIRCLE);
        lv_obj_set_size(dots_[i], at(POINT), at(POINT));
        if (clock) {
            lv_obj_set_pos(dots_[i], at(2 * PITCH + 1), at(i == 0 ? 30 : 72));
        } else {
            lv_obj_set_pos(dots_[i], at(166), at(96));
        }
    }
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

void SegmentDisplay::set_clock(int minutes, int seconds)
{
    minutes = std::clamp(minutes, 0, 99);
    seconds = std::clamp(seconds, 0, 59);
    set_digit(0, minutes / 10);
    set_digit(1, minutes % 10);
    set_digit(2, seconds / 10);
    set_digit(3, seconds % 10);
}

void SegmentDisplay::set_colon(bool on)
{
    for (lv_obj_t *dot : dots_) {
        if (dot != nullptr) {
            lv_obj_set_style_bg_opa(dot, on ? LV_OPA_COVER : OFF_OPACITY, 0);
        }
    }
}

void SegmentDisplay::set_ink(bool accent, std::uint32_t colour)
{
    for (Digit &digit : digits_) {
        for (lv_obj_t *bar : digit.bars) {
            theme::fill_accent_or(bar, accent, colour);
        }
    }
    for (lv_obj_t *dot : dots_) {
        if (dot != nullptr) {
            theme::fill_accent_or(dot, accent, colour);
        }
    }
}

void SegmentDisplay::set_tenths(int tenths)
{
    if (tenths < 0) {
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
