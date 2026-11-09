#pragma once

#include "esp_err.h"
#include "laptop_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

// Desk Link: a laptop's Now Playing and its Claude Code sessions, told to the
// panel's server sealed, and play, pause, seek, skip and volume sent back the
// same way. Each side pings the other, so each knows the other is there.
// Several laptops may share at once: the panel follows whichever plays, as
// laptops.h has it, and the commands go to that one.
namespace laptop {
/** The laptop the clicker's next and back go to, and its slide where it can tell. */
struct Clicker {
    std::string machine;  // empty for none
    int         page  = 0;  // from 1; 0 when it cannot tell
    int         pages = 0;
    std::string deck;
    int         presenting = -1;  // in the viewer's full screen; -1 when it cannot tell
    bool        shows      = false;  // the laptop puts it in and out of that

    bool operator==(const Clicker &) const = default;
};

struct Handlers {
    void (*playing)(const NowPlaying &now) = nullptr;
    // a tools/claude-hook event; quiet, shown without a chime or a notice
    void (*claude)(const char *event, std::size_t length, bool quiet) = nullptr;
    void (*sharing)(int laptops) = nullptr;  // how many share, as one joins or goes quiet
    // Stand or Sit tapped from a laptop, or None only to ask; where it stands and goes after
    DeskState (*desk)(Desk tap) = nullptr;
    void (*slides)(const Clicker &clicker) = nullptr;
};

/** On the update server, which must be up. The handlers run on its task, and
 *  `playing` and `sharing` on a timer's once a laptop has gone quiet. */
esp_err_t start(Handlers handlers);

void play_pause();
void seek(int position_s);
void next();
void previous();
void set_volume(int percent);
void set_muted(bool muted);
void step_volume(bool up);
/** The clicker's next or back, to the laptop slides_origin() names. */
void slide(bool next);
/** The viewer's full screen in or out, there. */
void slide_show();

/** Where a picture the laptop named, art or square, is for media to ask
 *  fetch_cover() for, or empty. */
std::string art_url(const std::string &art);

/** The cover at `url`, as art_url() gave it, opened into `into`; its length,
 *  or 0 when it could not be had. For media's fetcher, on its task. */
std::size_t fetch_cover(const char *url, std::uint8_t *into, std::size_t size);
inline constexpr char kCoverScheme[] = "desklink:";
}  // namespace laptop
