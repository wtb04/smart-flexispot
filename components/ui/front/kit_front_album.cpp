// What plays, in its cover's colours: the music view and the media card are
// coloured by the cover's most vivid hue rather than wrapped in a blur of it.

#include "kit_front.h"

#include <algorithm>
#include <cstdint>

namespace ui::kitfront {
namespace {
constexpr int     STEP          = 7;   // every so many pixels of the cover
constexpr int     VIVID_CHROMA  = 24;  // of 255: below it a pixel counts as grey
constexpr int     VIVID_SHARE   = 8;   // percent of the pixels that must have a colour
constexpr uint8_t ACCENT_S_MIN = 45, ACCENT_S_MAX = 75, ACCENT_V = 96, ACCENT_S_STEP = 5, ACCENT_V_STEP = 3;
constexpr int     ACCENT_LUMA     = 140;  // of 255: as light as this, to read on the dark
constexpr int     ACCENT_LUMA_MAX = 175;  // and no lighter, for the glyph on it
}  // namespace

AlbumColours album_colours(const std::uint16_t *pixels, int count, bool dark)
{
    AlbumColours out;
    if (pixels == nullptr || count <= 0) {
        return out;
    }
    // each pixel weighted by how much colour it has, so its greys and blacks
    // do not wash the hue out; a cover with little colour keeps the page's own
    std::uint64_t r = 0, g = 0, b = 0, weight = 0;
    int           vivid = 0, seen = 0;
    for (int i = 0; i < count; i += STEP, ++seen) {
        const std::uint16_t v  = pixels[i];
        const int           pr = ((v >> 11) & 0x1f) * 255 / 31;
        const int           pg = ((v >> 5) & 0x3f) * 255 / 63;
        const int           pb = (v & 0x1f) * 255 / 31;
        const int           chroma = std::max({pr, pg, pb}) - std::min({pr, pg, pb});
        if (chroma < VIVID_CHROMA) {
            continue;
        }
        ++vivid;
        r += static_cast<std::uint64_t>(pr) * chroma;
        g += static_cast<std::uint64_t>(pg) * chroma;
        b += static_cast<std::uint64_t>(pb) * chroma;
        weight += chroma;
    }
    if (seen == 0 || vivid * 100 < seen * VIVID_SHARE) {
        return out;
    }
    const lv_color_hsv_t hue = lv_color_rgb_to_hsv(static_cast<std::uint8_t>(r / weight),
                                                   static_cast<std::uint8_t>(g / weight),
                                                   static_cast<std::uint8_t>(b / weight));
    // Blue and violet are dark at any brightness: paler until they read.
    // Yellow is light at any: darker until the glyph on it does.
    std::uint8_t saturation = std::clamp<std::uint8_t>(hue.s, ACCENT_S_MIN, ACCENT_S_MAX);
    std::uint8_t value      = ACCENT_V;
    lv_color_t   accent     = lv_color_hsv_to_rgb(hue.h, saturation, value);
    while (lv_color_luminance(accent) < ACCENT_LUMA && saturation > ACCENT_S_STEP) {
        saturation -= ACCENT_S_STEP;
        accent = lv_color_hsv_to_rgb(hue.h, saturation, value);
    }
    while (lv_color_luminance(accent) > ACCENT_LUMA_MAX && value > ACCENT_V_STEP) {
        value -= ACCENT_V_STEP;
        accent = lv_color_hsv_to_rgb(hue.h, saturation, value);
    }
    out.vivid   = true;
    out.accent  = accent;
    out.back    = dark ? lv_color_hsv_to_rgb(hue.h, 70, 24) : lv_color_hsv_to_rgb(hue.h, 12, 96);
    out.surface = dark ? lv_color_hsv_to_rgb(hue.h, 50, 36) : lv_color_hsv_to_rgb(hue.h, 14, 98);
    out.ink     = dark ? accent : lv_color_hsv_to_rgb(hue.h, 70, 42);
    return out;
}

}  // namespace ui::kitfront
