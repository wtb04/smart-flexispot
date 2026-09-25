#pragma once

#include "esp_err.h"
#include "units.h"

#include <cstddef>
#include <cstdint>

namespace jpeg {
/** The panel has one JPEG engine; this component owns it, its buffers and the
 *  software fallback for what the engine will not take. */
inline constexpr std::size_t kMaxInput = 512 * units::kBytesPerKiB;
inline constexpr int         kMaxSide  = 800;

/** A decoded picture. Valid only inside the callback that is handed it. */
struct Picture {
    const void *pixels;  // RGB565, or one byte per pixel when grey
    int         width;
    int         height;
    int         stride;  // in pixels: the engine pads each row to whole blocks
    bool        grey;
    bool        hardware;  // the engine's work, rather than the software fallback
};

using Use = void (*)(const Picture &picture, void *context);

esp_err_t start();

/** Decodes and hands the picture to `use`, holding the engine meanwhile so
 *  nothing else can overwrite it. A picture bigger than the box is decoded in
 *  software at a half, quarter or eighth of its size until it fits, and
 *  refused if even that will not. From any task; one decode at a time.
 *
 *  The software path runs on the caller's stack and needs several kilobytes
 *  of it: give a task that decodes 8 KB or more. */
bool decode(const void *data, std::size_t length, int max_w, int max_h, Use use, void *context);

/** The same, copied out as RGB565 into `out`, which holds max_w * max_h pixels
 *  and is read with rows of out_w. */
bool decode_into(const void *data, std::size_t length, std::uint16_t *out, int max_w, int max_h,
                 int &out_w, int &out_h);

/** Built arithmetically, so unlike the engine's own output it needs no byte swap. */
constexpr std::uint16_t grey_to_rgb565(std::uint8_t level)
{
    constexpr int RED_SHIFT   = 11;
    constexpr int GREEN_SHIFT = 5;
    const int     five_bits   = level >> 3;  // of red and of blue
    const int     six_bits    = level >> 2;  // of green
    return static_cast<std::uint16_t>((five_bits << RED_SHIFT) | (six_bits << GREEN_SHIFT) |
                                      five_bits);
}

}  // namespace jpeg
