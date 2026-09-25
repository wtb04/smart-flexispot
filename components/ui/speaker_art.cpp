#include "ui_internal.h"

#include "esp_heap_caps.h"

namespace ui::detail {
namespace {
// A Harman Kardon Citation One from the front, measured off a photograph: a
// fabric-covered body whose flat top shows as half an oval, seen from just
// above; four lights under it and the maker's plate low on the front. Heights
// are shares of the speaker's height, widths of its width.
constexpr float HEIGHT     = 0.86f;   // of the picture
constexpr float WIDTH      = 0.69f;   // of its own height
constexpr float BOTTOM_R   = 0.32f;
constexpr float TOP_RY     = 0.05f;
constexpr float DOTS_Y     = 0.14f;
constexpr float DOT_D      = 0.026f;
constexpr float DOT_GAP    = 0.06f;
constexpr int   DOTS       = 4;
constexpr float PLATE_Y    = 0.91f;
constexpr float PLATE_W    = 0.28f;
constexpr float PLATE_H    = 0.025f;
constexpr float FABRIC     = 0.40f;   // how much of the secondary colour shows through
constexpr float TOP_FACE   = 0.62f;
constexpr float DOT_INK    = 0.70f;
constexpr float PLATE_INK  = 0.60f;
constexpr int   SUBSAMPLES = 3;       // a side, so the curves do not stair-step

struct Rgb {
    float r, g, b;
};

Rgb rgb(std::uint32_t colour)
{
    constexpr float MAX = 255.0f;
    return {static_cast<float>((colour >> 16) & 0xff) / MAX,
            static_cast<float>((colour >> 8) & 0xff) / MAX,
            static_cast<float>(colour & 0xff) / MAX};
}

Rgb mix(Rgb a, Rgb b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

std::uint16_t rgb565(Rgb c)
{
    const auto part = [](float v, int bits) {
        return static_cast<std::uint16_t>(v * static_cast<float>((1 << bits) - 1) + 0.5f);
    };
    return static_cast<std::uint16_t>(part(c.r, 5) << 11 | part(c.g, 6) << 5 | part(c.b, 5));
}

bool in_rounded(float x, float y, float x0, float y0, float x1, float y1, float r)
{
    if (x < x0 || x > x1 || y < y0 || y > y1) {
        return false;
    }
    const float cx = std::min(std::max(x, x0 + r), x1 - r);
    const float cy = std::min(std::max(y, y0 + r), y1 - r);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

bool in_oval(float x, float y, float cx, float cy, float rx, float ry)
{
    const float dx = (x - cx) / rx;
    const float dy = (y - cy) / ry;
    return dx * dx + dy * dy <= 1.0f;
}

struct Speaker {
    float x0, y0, w, h, r, ry;
};

Rgb colour_at(const Speaker &s, float x, float y)
{
    const Rgb panel = rgb(theme::panel);
    const Rgb ink   = rgb(theme::text);
    const Rgb shade = rgb(theme::secondary);

    const bool top  = in_oval(x, y, s.x0 + s.w / 2, s.y0 + s.ry, s.w / 2, s.ry);
    // Square where it meets the top, round only at the bottom.
    const bool body = in_rounded(x, y, s.x0, s.y0 + s.ry, s.x0 + s.w, s.y0 + s.h, s.r) ||
                      (x >= s.x0 && x <= s.x0 + s.w && y >= s.y0 + s.ry && y <= s.y0 + s.ry + s.r);
    if (!top && !body) {
        return panel;
    }
    Rgb c = mix(panel, shade, top ? TOP_FACE : FABRIC);

    const float dot_r = s.w * DOT_D / 2;
    for (int i = 0; i < DOTS; ++i) {
        const float offset = (static_cast<float>(i) - static_cast<float>(DOTS - 1) / 2) * s.w * DOT_GAP;
        if (in_oval(x, y, s.x0 + s.w / 2 + offset, s.y0 + s.h * DOTS_Y, dot_r, dot_r)) {
            c = mix(c, ink, DOT_INK);
        }
    }
    if (std::abs(y - (s.y0 + s.h * PLATE_Y)) <= s.h * PLATE_H / 2 &&
        std::abs(x - (s.x0 + s.w / 2)) <= s.w * PLATE_W / 2) {
        c = mix(c, ink, PLATE_INK);
    }
    return c;
}
}  // namespace

const lv_image_dsc_t *speaker_picture(std::int32_t side)
{
    static lv_image_dsc_t dsc{};
    static std::uint16_t *pixels = nullptr;
    if (pixels != nullptr) {
        return &dsc;
    }
    const std::size_t count = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    pixels = static_cast<std::uint16_t *>(heap_caps_malloc(count * sizeof(std::uint16_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr) {
        return nullptr;
    }

    const float   f = static_cast<float>(side);
    Speaker       s{};
    s.h  = f * HEIGHT;
    s.w  = s.h * WIDTH;
    s.x0 = (f - s.w) / 2;
    s.y0 = (f - s.h) / 2;
    s.r  = s.w * BOTTOM_R;
    s.ry = s.h * TOP_RY;

    constexpr float SHARE = 1.0f / (SUBSAMPLES * SUBSAMPLES);
    for (std::int32_t py = 0; py < side; ++py) {
        for (std::int32_t px = 0; px < side; ++px) {
            Rgb sum{0, 0, 0};
            for (int sy = 0; sy < SUBSAMPLES; ++sy) {
                for (int sx = 0; sx < SUBSAMPLES; ++sx) {
                    const Rgb c = colour_at(
                        s, static_cast<float>(px) + (static_cast<float>(sx) + 0.5f) / SUBSAMPLES,
                        static_cast<float>(py) + (static_cast<float>(sy) + 0.5f) / SUBSAMPLES);
                    sum = {sum.r + c.r * SHARE, sum.g + c.g * SHARE, sum.b + c.b * SHARE};
                }
            }
            pixels[static_cast<std::size_t>(py) * side + px] = rgb565(sum);
        }
    }

    const std::uint32_t bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc.header.w      = side;
    dsc.header.h      = side;
    dsc.header.stride = side * bytes;
    dsc.data_size     = count * bytes;
    dsc.data          = reinterpret_cast<const std::uint8_t *>(pixels);
    return &dsc;
}
}  // namespace ui::detail
