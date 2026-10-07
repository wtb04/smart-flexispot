#pragma once

#include <algorithm>
#include <cstdint>

namespace ui::kit::pixels {

// RGB565 has 32 steps of red and blue and 64 of green, too few for a soft dark
// gradient, which then breaks into flat blobs with edges. Rounding each pixel
// up or down by a fixed 4x4 pattern spreads the steps into a texture too fine
// to see at this screen's density.
inline constexpr std::uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

inline std::uint16_t dither(int r, int g, int b, int x, int y)
{
    const int t = BAYER[y & 3][x & 3];
    r = std::min(255, r + t / 2);  // a red or blue step is 8, half a Bayer step
    g = std::min(255, g + t / 4);  // a green one is 4
    b = std::min(255, b + t / 2);
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/** The same as dither(), from tables: for whole screens of pixels from the PPA, B, G, R. */
class Ditherer {
  public:
    Ditherer()
    {
        for (int t = 0; t < 16; ++t) {
            for (int v = 0; v < 256; ++v) {
                m_r[t][v] = static_cast<std::uint8_t>(std::min(255, v + t / 2) >> 3);
                m_g[t][v] = static_cast<std::uint8_t>(std::min(255, v + t / 4) >> 2);
                m_b[t][v] = static_cast<std::uint8_t>(std::min(255, v + t / 2) >> 3);
            }
        }
    }
    void row(const std::uint8_t *in, std::uint16_t *out, int width, int y) const
    {
        const std::uint8_t *pattern = BAYER[y & 3];
        const std::uint8_t *r[4], *g[4], *b[4];
        for (int i = 0; i < 4; ++i) {
            r[i] = m_r[pattern[i]];
            g[i] = m_g[pattern[i]];
            b[i] = m_b[pattern[i]];
        }
        int x = 0;
        for (; x + 4 <= width; x += 4, in += 12) {  // four pixels a step, the pattern's width
            out[x + 0] = static_cast<std::uint16_t>(r[0][in[2]] << 11 | g[0][in[1]] << 5 | b[0][in[0]]);
            out[x + 1] = static_cast<std::uint16_t>(r[1][in[5]] << 11 | g[1][in[4]] << 5 | b[1][in[3]]);
            out[x + 2] = static_cast<std::uint16_t>(r[2][in[8]] << 11 | g[2][in[7]] << 5 | b[2][in[6]]);
            out[x + 3] = static_cast<std::uint16_t>(r[3][in[11]] << 11 | g[3][in[10]] << 5 | b[3][in[9]]);
        }
        for (; x < width; ++x, in += 3) {
            out[x] = static_cast<std::uint16_t>(r[x & 3][in[2]] << 11 | g[x & 3][in[1]] << 5 | b[x & 3][in[0]]);
        }
    }

  private:
    std::uint8_t m_r[16][256], m_g[16][256], m_b[16][256];
};

}  // namespace ui::kit::pixels
