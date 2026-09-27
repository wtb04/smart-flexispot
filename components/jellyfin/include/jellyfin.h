#pragma once

#include "esp_err.h"
#include "jellyfin_protocol.h"

#include <string>

// A live line to Jellyfin: what its sessions play, pushed as it changes, and
// pause and seek sent straight to it. Home Assistant only polls the server, so
// a pause on the laptop took it the better part of half a minute to notice.
namespace jellyfin {
/** Each change in what the followed session plays, from the socket's task. */
using Handler = void (*)(const NowPlaying &now);

/** Safe before Wi-Fi is up: the client keeps trying on its own. Does nothing
 *  without an address and key in jellyfin_secrets.h. */
esp_err_t start(Handler on_change);

/** For the session being followed; from any task, returning at once. */
void play_pause();
void seek(int position_s);
void set_volume(int percent);
void toggle_subtitles();  // off, or on with the default track
void play_now(const std::string &item);

/** Reads `path` on the server with the key, blocking; false when it fails. */
bool fetch(const std::string &path, std::string &out);

/** The whole address of `item`'s cover, for the cover fetcher. */
std::string cover_url(const std::string &item, int height);
}  // namespace jellyfin
