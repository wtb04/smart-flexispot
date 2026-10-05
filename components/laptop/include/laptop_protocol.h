#pragma once

#include <cstddef>
#include <string>

// What Desk Link, on a laptop, says is playing there, and what the panel asks
// of it back. Pure, so the host tests can run it.
//
// The laptop posts a report to the panel's /laptop whenever what plays changes
// and every few seconds while anything does:
//   {"machine": "wtb-mbp", "port": 47801, "app": "Safari", "playing": true,
//    "title": "...", "artist": "...", "position": 167.4, "duration": 540,
//    "art": "3f2a", "volume": 40, "muted": false,
//    "takes": ["pause", "seek", "next", "previous", "volume"]}
// An empty title is nothing playing. "art", when there is any, changes with
// the picture, which is at /art.jpg on the laptop's port. "volume" is the
// laptop's own output, in percent, where it can be set. Commands go to
// /command there: {"command": "pause"}, "seek" with a "position", "volume"
// with a "level", "mute" with "muted".
namespace laptop {
struct NowPlaying {
    bool        active  = false;  // something to show, playing or paused
    bool        playing = false;
    std::string machine;
    int         port = 0;
    std::string app;
    std::string title;
    std::string artist;
    std::string art;
    int         position_s = 0;
    int         duration_s = 0;
    int         volume     = -1;  // percent, -1 when it has none
    bool        muted      = false;
    bool        takes_pause    = false;
    bool        takes_seek     = false;
    bool        takes_next     = false;
    bool        takes_previous = false;
    bool        takes_volume   = false;
};

/** False when `body` is not a report: unreadable, or without a port to answer on. */
bool read(const char *body, std::size_t length, NowPlaying &out);

enum class Command { Play, Pause, Next, Previous, Seek, Volume, Mute };
/** `value` is the position for Seek, the percent for Volume, 1 to mute for Mute. */
std::string command_body(Command command, int value = 0);

/** Where the laptop serves the picture, versioned so a new one is fetched. */
std::string art_path(const std::string &art);
}  // namespace laptop
