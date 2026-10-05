#pragma once

#include "esp_err.h"
#include "laptop_protocol.h"

#include <string>

// A laptop's Now Playing, as Desk Link reports it to the panel's server, and
// play, pause, seek and skip sent back to it.
namespace laptop {
using Handler = void (*)(const NowPlaying &now);

/** On the update server, which must be up. `on_change` runs on its task, and
 *  on a timer's once a laptop has gone quiet. */
esp_err_t start(Handler on_change);

void play_pause();
void seek(int position_s);
void next();
void previous();
void set_volume(int percent);
void set_muted(bool muted);

/** The picture's whole address, or empty when there is none. */
std::string art_url(const NowPlaying &now);
}  // namespace laptop
