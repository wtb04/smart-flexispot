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

/** What the cover fetcher last managed, for the diagnostics page. */
struct Status {
    bool playing;
    bool have_art;
    bool art_ok;
    bool hardware;   // the part's own engine, rather than the software fallback
    int  decode_ms;
    int  decodes;
};

/** Thread-safe. */
Status status();

/** Requires the network to be up, and jpeg::start. Covers are fetched from
 *  `origin`, such as "http://10.0.0.2:8123", which Home Assistant serves them on. */
esp_err_t start(const char *origin, ArtHandler on_art);


/**
 * Thread-safe. Takes the entity_picture_local proxy path, which Home Assistant
 * serves over plain HTTP with a token in the query, so no TLS is needed. An
 * empty path clears the art.
 */
void set_art_path(const char *path);

}  // namespace media
