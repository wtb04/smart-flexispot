#pragma once

#include "esp_err.h"
#include "laptop_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Desk Link: a laptop's Now Playing and its Claude Code sessions, told to the
// panel's server sealed, and play, pause, seek, skip and volume sent back the
// same way. Each side pings the other, so each knows the other is there.
namespace laptop {
struct Handlers {
    void (*playing)(const NowPlaying &now) = nullptr;
    void (*claude)(const char *event, std::size_t length) = nullptr;  // a tools/claude-hook event
};

/** On the update server, which must be up. The handlers run on its task, and
 *  `playing` on a timer's once a laptop has gone quiet. */
esp_err_t start(Handlers handlers);

void play_pause();
void seek(int position_s);
void next();
void previous();
void set_volume(int percent);
void set_muted(bool muted);
void step_volume(bool up);

/** Where a picture the laptop named, art or square, is for media to ask
 *  fetch_cover() for, or empty. */
std::string art_url(const std::string &art);

/** The cover at `url`, as art_url() gave it, opened into `into`; its length,
 *  or 0 when it could not be had. For media's fetcher, on its task. */
std::size_t fetch_cover(const char *url, std::uint8_t *into, std::size_t size);
inline constexpr char kCoverScheme[] = "desklink:";
}  // namespace laptop
