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
//    "art": "3f2a", "square": "9c1e", "video": true, "volume": 40, "muted": false,
//    "takes": ["pause", "seek", "next", "previous", "volume"]}
// An empty title is nothing playing. "art", when there is any, changes with
// the picture, which the panel asks the laptop's /cover for; "square" is one
// for where a square is drawn, as a channel's picture beside its video's frame,
// asked for the same way. "volume" is the
// laptop's own output, in percent, where it can be set; "volume_step" in
// "takes" is an output that only steps, as a display's does through
// MonitorControl, moved by "volume_up" and "volume_down". "slides" in
// "takes" is a laptop that presses Page Down and Page Up for "slide_next" and
// "slide_back", as a presentation clicker does, whatever plays; "slide", when
// it can read the viewer the keys go to, is where that is:
//   "slide": {"page": 4, "pages": 12, "deck": "talk.pdf", "presenting": false}
// "presenting" left out where it cannot tell. "slide_show" in "takes" is one
// that puts the viewer in and out of its full screen for "slide_show". "claude" carries an
// event from tools/claude-hook as "event", and "quiet": true when the laptop
// wants its sessions shown without a chime or a notice. "desk" with a "preset",
// "stand" or "sit", taps that preset as the screen's button does, and without
// one only asks. It is answered "desk", and "pong" says the same, whenever
// there is a desk to move: where it stands, "desk": "stand", "sit" or
// "between", "going": "stand" or "sit" while on its way, and "height" in
// millimetres once the desk has said it. The panel sends the laptop's /link
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
    std::string square;
    bool        video = false;  // a film or a clip rather than a song: the cinema shows it
    int         position_s = 0;
    int         duration_s = 0;
    int         volume     = -1;  // percent, -1 when it has none
    bool        muted      = false;
    bool        takes_pause    = false;
    bool        takes_next     = false;
    bool        takes_previous = false;
    bool        takes_volume   = false;
    bool        takes_volume_step = false;  // up and down, with no level to show
    bool        takes_seek     = false;     // also a live stream's, which has no length
    bool        takes_slides   = false;     // the clicker's next and back, playing or not
    bool        takes_slide_show = false;   // the viewer's full screen, in and out
    int         slide_page  = 0;            // from 1; 0 when the laptop cannot tell
    int         slide_pages = 0;
    std::string slide_deck;
    int         slide_presenting = -1;      // in its full screen; -1 when the laptop cannot tell
};

/** A message's type and when it was sent; false when it has neither. */
struct Head {
    std::string  type;
    std::int64_t at_ms = 0;
};
bool read_head(const char *body, std::size_t length, Head &out);

/** A state's now playing; false without a port to answer on. */
bool read(const char *body, std::size_t length, NowPlaying &out);

/** The hook's event a "claude" message carries, as JSON; empty for none. With
 *  `quiet`, whether the laptop asked for no chime and no notice. */
std::string claude_event(const char *body, std::size_t length, bool *quiet = nullptr);

/** None is no desk to move, or nowhere it goes. */
enum class Desk : std::uint8_t { None, Between, Stand, Sit };

/** "stand", "sit", "between", or empty for None. */
const char *desk_name(Desk desk);

struct DeskState {
    Desk at    = Desk::None;  // None without a desk linked
    Desk going = Desk::None;  // Stand or Sit while it is on its way there
    int  height_mm = -1;      // negative while the desk has not said
};

/** Which preset a "desk" message taps, into `out`: Stand, Sit, or None when it
 *  only asks. False when it names something else. */
bool desk_preset(const char *body, std::size_t length, Desk &out);

enum class Command { Play, Pause, Next, Previous, Seek, Volume, Mute, VolumeUp, VolumeDown, SlideNext, SlideBack, SlideShow };
/** `value` is the position for Seek, the percent for Volume, 1 to mute for Mute. */
std::string command_body(Command command, int value, std::int64_t at_ms);

/** `reaches`: -1 before the panel has tried the laptop's port, else whether it answered. */
std::string pong_body(const char *panel, const char *firmware, int reaches, std::int64_t at_ms,
                      DeskState desk = {});
std::string desk_body(DeskState desk, std::int64_t at_ms);
std::string ping_body(std::int64_t at_ms);
std::string ok_body(std::int64_t at_ms);
std::string cover_body(const std::string &art, std::int64_t at_ms);
}  // namespace laptop
