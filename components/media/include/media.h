#pragma once

#include "esp_err.h"

#include <cstdint>

namespace media {
/** Pixels. Sized for the largest place it is shown, so nothing scales up. */
inline constexpr int kArtSize = 200;

/** Favourites offered while nothing plays, and their covers' side. */
inline constexpr int kPickCount   = 8;
inline constexpr int kPickArtSize = 180;

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

/** A favourite's cover, kPickArtSize squared of RGB565, or null for none; from
 *  the fetch task. The buffer is that favourite's own, rewritten in place. */
using PickArtHandler = void (*)(int index, const void *pixels);

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
esp_err_t start(const char *origin, ArtHandler on_art, PickArtHandler on_pick_art);

/**
 * Thread-safe. Takes the entity_picture_local proxy path, which Home Assistant
 * serves over plain HTTP with a token in the query, or a whole address, as
 * Jellyfin's covers have. An empty path clears the art.
 */
void set_art_path(const char *path);

/** Thread-safe. A favourite's cover by its whole address, HTTPS included, fetched
 *  once the playing cover is in hand. Empty clears it. */
void set_pick_art(int index, const char *url);

}  // namespace media
