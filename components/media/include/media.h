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

/**
 * Thread-safe. Takes the entity_picture_local proxy path, which Home Assistant
 * serves over plain HTTP with a token in the query, so no TLS is needed. An
 * empty path clears the art.
 */
void set_art_path(const char *path);

}  // namespace media
