#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// What Desk Link, on a laptop, says to the panel and the panel to it, each
// message sealed as link_envelope.h has it. Pure, so the host tests can run it.
//
// The laptop says how it is, "state", to the panel's /link whenever what
// plays changes and every ten seconds whatever it is, and the panel answers
// "pong": that it is the panel, and whether it reaches the laptop back.
//   {"type": "state", "at": 1791223792000, "machine": "wtb-mbp", "port": 47801,
//    "app": "Safari", "playing": true,
//    "title": "...", "artist": "...", "position": 167.4, "duration": 540,
//    "art": "3f2a", "video": true, "volume": 40, "muted": false,
//    "takes": ["pause", "seek", "next", "previous", "volume"]}
// An empty title is nothing playing. "art", when there is any, changes with
// the picture, which the panel asks the laptop's /cover for. "volume" is the
// laptop's own output, in percent, where it can be set. "claude" carries an
// event from tools/claude-hook as "event". The panel sends the laptop's /link
// "ping", and "command": "pause", "seek" with a "position", "volume" with a
// "level", "mute" with "muted". What answers is sealed as well: "pong", "ok",
// and for a cover the picture itself.
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
    bool        video = false;  // a film or a clip rather than a song: the cinema shows it
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

/** A message's type and when it was sent; false when it has neither. */
struct Head {
    std::string  type;
    std::int64_t at_ms = 0;
};
bool read_head(const char *body, std::size_t length, Head &out);

/** A state's now playing; false without a port to answer on. */
bool read(const char *body, std::size_t length, NowPlaying &out);

/** The hook's event a "claude" message carries, as JSON; empty for none. */
std::string claude_event(const char *body, std::size_t length);

enum class Command { Play, Pause, Next, Previous, Seek, Volume, Mute };
/** `value` is the position for Seek, the percent for Volume, 1 to mute for Mute. */
std::string command_body(Command command, int value, std::int64_t at_ms);

/** `reaches`: -1 before the panel has tried the laptop's port, else whether it answered. */
std::string pong_body(const char *panel, const char *firmware, int reaches, std::int64_t at_ms);
std::string ping_body(std::int64_t at_ms);
std::string ok_body(std::int64_t at_ms);
std::string cover_body(const std::string &art, std::int64_t at_ms);
}  // namespace laptop
