// The speaker the music plays on, for the cover's well while nothing does: a
// Harman Kardon Citation One from the front, drawn once per size as a mask the
// well's ink colours, so it takes the light look and the dark alike.

#include "kit_front.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ui::kitfront {
namespace {
// Measured off a photograph: a fabric-covered body whose flat top shows as half
// an oval, seen from just above; four lights under it and the maker's plate low
// on the front. Heights are shares of the speaker's height, widths of its width.
constexpr float HEIGHT     = 0.86f;  // of the picture
constexpr float WIDTH      = 0.69f;  // of its own height
constexpr float BOTTOM_R   = 0.32f;
constexpr float TOP_RY     = 0.05f;
constexpr float DOTS_Y     = 0.14f;
constexpr float DOT_D      = 0.026f;
constexpr float DOT_GAP    = 0.06f;
constexpr int   DOTS       = 4;
constexpr float PLATE_Y    = 0.91f;
constexpr float PLATE_W    = 0.28f;
constexpr float PLATE_H    = 0.025f;
constexpr float FABRIC     = 0.45f;  // how much ink each part takes
constexpr float TOP_FACE   = 0.75f;
constexpr float LIGHTS     = 1.0f;
constexpr int   SUBSAMPLES = 3;      // a side, so the curves do not stair-step
constexpr int   SIZES      = 2;      // the bar's and the media card's

struct Speaker {
    float x0, y0, w, h, r, ry;
};

bool in_rounded(float x, float y, float x0, float y0, float x1, float y1, float r)
{
    if (x < x0 || x > x1 || y < y0 || y > y1) {
        return false;
    }
    const float cx = std::clamp(x, x0 + r, x1 - r);
    const float cy = std::clamp(y, y0 + r, y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

bool in_oval(float x, float y, float cx, float cy, float rx, float ry)
{
    const float dx = (x - cx) / rx, dy = (y - cy) / ry;
    return dx * dx + dy * dy <= 1.0f;
}

float ink_at(const Speaker &s, float x, float y)
{
    const bool top = in_oval(x, y, s.x0 + s.w / 2, s.y0 + s.ry, s.w / 2, s.ry);
    // square where it meets the top, round only at the bottom
    const bool body = in_rounded(x, y, s.x0, s.y0 + s.ry, s.x0 + s.w, s.y0 + s.h, s.r) ||
                      (x >= s.x0 && x <= s.x0 + s.w && y >= s.y0 + s.ry && y <= s.y0 + s.ry + s.r);
    if (!top && !body) {
        return 0.0f;
    }
    const float dot_r = s.w * DOT_D / 2;
    for (int i = 0; i < DOTS; ++i) {
        const float offset = (static_cast<float>(i) - static_cast<float>(DOTS - 1) / 2) * s.w * DOT_GAP;
        if (in_oval(x, y, s.x0 + s.w / 2 + offset, s.y0 + s.h * DOTS_Y, dot_r, dot_r)) {
            return LIGHTS;
        }
    }
    if (std::abs(y - (s.y0 + s.h * PLATE_Y)) <= s.h * PLATE_H / 2 && std::abs(x - (s.x0 + s.w / 2)) <= s.w * PLATE_W / 2) {
        return LIGHTS;
    }
    return top ? TOP_FACE : FABRIC;
}

struct Made {
    std::int32_t   side = 0;
    lv_image_dsc_t dsc{};
};
Made s_made[SIZES];
}  // namespace

const lv_image_dsc_t *speaker_mark(std::int32_t side)
{
    Made *slot = nullptr;
    for (Made &m : s_made) {
        if (m.side == side) {
            return &m.dsc;
        }
        if (slot == nullptr && m.side == 0) {
            slot = &m;
        }
    }
    const std::size_t count  = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    auto             *pixels = slot != nullptr ? static_cast<std::uint8_t *>(
                                                     heap_caps_malloc(count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
                                               : nullptr;
    if (pixels == nullptr) {
        return nullptr;
    }
    const float f = static_cast<float>(side);
    Speaker     s{};
    s.h  = f * HEIGHT;
    s.w  = s.h * WIDTH;
    s.x0 = (f - s.w) / 2;
    s.y0 = (f - s.h) / 2;
    s.r  = s.w * BOTTOM_R;
    s.ry = s.h * TOP_RY;
    constexpr float SHARE = 1.0f / (SUBSAMPLES * SUBSAMPLES);
    for (std::int32_t py = 0; py < side; ++py) {
        for (std::int32_t px = 0; px < side; ++px) {
            float sum = 0.0f;
            for (int sy = 0; sy < SUBSAMPLES; ++sy) {
                for (int sx = 0; sx < SUBSAMPLES; ++sx) {
                    sum += ink_at(s, static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / SUBSAMPLES,
                                  static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / SUBSAMPLES) *
                           SHARE;
                }
            }
            pixels[static_cast<std::size_t>(py) * side + px] = static_cast<std::uint8_t>(sum * 255.0f + 0.5f);
        }
    }
    slot->side                  = side;
    slot->dsc.header.magic      = LV_IMAGE_HEADER_MAGIC;
    slot->dsc.header.cf         = LV_COLOR_FORMAT_A8;
    slot->dsc.header.w          = side;
    slot->dsc.header.h          = side;
    slot->dsc.header.stride     = side;
    slot->dsc.data_size         = count;
    slot->dsc.data              = pixels;
    return &slot->dsc;
}

}  // namespace ui::kitfront
