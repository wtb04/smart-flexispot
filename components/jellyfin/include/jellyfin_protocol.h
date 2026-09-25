#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Jellyfin's side of the talk: the messages to and from its websocket, and the
// addresses its API takes commands and hands out covers at. Pure, so the host
// tests can run it.
namespace jellyfin {
/** What a session is playing, as its latest report has it. */
struct NowPlaying {
    bool        active = false;  // something playing or paused
    bool        paused = false;
    std::string session;         // the session's id, which commands go to
    std::string item;            // what plays
    std::string kind;            // Episode, Movie, Audio and so on
    std::string title;
    std::string series;          // for an episode
    std::string series_id;       // whose poster stands for the episode
    std::string season_id;       // or the season's, first
    int         season     = 0;
    int         episode    = 0;
    int         position_s = 0;
    int         duration_s = 0;
};

/** Keeps a Sessions message small as it arrives: the queue's full items run to
 *  most of a megabyte for a season, and are dropped, left as null, before any
 *  of them is kept. Fed the message in the pieces it comes in. */
class Trimmer {
public:
    void reset();
    void feed(const char *data, std::size_t size, std::string &out);

private:
    void pass(char c, std::string &out);
    void skip(char c, std::string &out);

    bool in_string_ = false;
    bool escaped_   = false;
    bool skipping_  = false;
    bool opened_    = false;  // the skipped value has begun
    int  depth_     = 0;
};

/** Asks for every session's state, pushed as it changes and at least each
 *  interval, rather than polled. */
std::string sessions_start(int interval_ms);

std::string keep_alive();

/** The message's type, such as Sessions or ForceKeepAlive; empty if unreadable. */
std::string message_type(const std::string &message);

/** From a Sessions message, the session to follow: a playing one before a
 *  paused one, never `own_device`'s. Not active when none plays anything. */
NowPlaying now_playing(const std::string &message, const std::string &own_device);

/** Where to post to toggle pause, or to jump, in `session`. */
std::string play_pause_path(const std::string &session);
std::string seek_path(const std::string &session, int position_s);

/** Where `item`'s cover is, `height` pixels tall, as a JPEG. */
std::string cover_path(const std::string &item, int height);
}  // namespace jellyfin
