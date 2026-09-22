#pragma once

#include "esp_err.h"

#include <cstdint>

namespace media {
/** Pixels. Sized for the largest place it is shown, so nothing scales up. */
inline constexpr int kArtSize = 200;

/**
 * RGB565, kArtSize squared, called from the fetch task. The buffer is valid
 * until the cover after next replaces it; null means there is no art.
 */
enum class Art : std::uint8_t {
    None,    // nothing is playing, or the track has no cover
    Ready,   // pixels point at kArtSize squared of RGB565
    Failed,  // there was a cover and it could not be had
};

using ArtHandler = void (*)(Art state, const void *pixels);

/** Requires the network to be up. */
esp_err_t start(ArtHandler on_art);

/** Decodes a JPEG at its own size into `out`, which must hold max_w * max_h
 *  pixels, and reports what came back. Anything larger than that box is
 *  refused rather than cropped.
 *
 *  The panel has one JPEG engine and this component owns it, so this is here
 *  rather than anywhere more fitting. It serialises against the cover fetch. */
bool decode_image(const void *jpeg, std::size_t length, std::uint16_t *out, int max_w, int max_h,
                  int &out_w, int &out_h);

/**
 * Thread-safe. Takes the entity_picture_local proxy path, which Home Assistant
 * serves over plain HTTP with a token in the query, so no TLS is needed. An
 * empty path clears the art.
 */
void set_art_path(const char *path);

}  // namespace media
