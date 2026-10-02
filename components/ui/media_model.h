#pragma once

#include "media.h"
#include "ui.h"

#include "freertos/FreeRTOS.h"

#include <cstdint>

// What plays, and what can be done with it: the data the media card, the music
// view and the cinema view all show, as one. Updates come in through the take_
// functions, which keep its rules (a pause shows once it has settled) and
// publish Topic::Media; the views read it and act on it through the commands
// below, never writing it themselves. On the LVGL task only.
namespace ui::detail {

struct MediaState {
    char source[32]  = "";
    char title[128]  = "";  // empty while nothing plays
    char artist[128] = "";  // for a video, the series, a newline, and its season and episode
    char state[24]   = "";  // what the player is, shown where the title goes while nothing plays
    bool has_track    = false;
    bool controllable = false;  // a player there is to steer; holding the card does nothing without one
    bool playing      = false;  // as shown: a pause shows once it has settled
    bool remote       = true;   // takes play, pause, skip and seek from here
    bool video        = false;  // it seeks, as a film or an episode does
    int  position_s   = 0;      // as last reported, at position_at
    int  duration_s   = 0;      // 0 while unknown
    bool advancing    = false;  // the position runs on from position_at
    TickType_t position_at = 0;
    int  volume       = -1;     // percent, as last reported or set, -1 before either

    const void   *art         = nullptr;  // the cover at media::kArtSize, RGB565, or null
    bool          placeholder = false;    // no cover will come: a blank where it would be
    const void   *large       = nullptr;  // the same cover at media::kLargeArtSize, or null
    std::uint32_t covers      = 0;        // counts every cover that came, as one may come into the same buffer

    // A video's own: its still, its subtitles, the episodes either side, its
    // intro and credits.
    const void   *still  = nullptr;  // media::kStillW by kStillH, RGB565, or null
    std::uint32_t stills = 0;
    bool subtitles_available = false;
    bool subtitles_shown     = false;
    bool before = false, after = false;
    MediaSegment segments[kMaxSegments]{};
    int          segment_count = 0;

    int hold_preset = -1;  // the desk preset holding the card goes to, or -1 for the music view
};

/** Read anywhere; written only here. */
const MediaState &media_state();

// ---- Updates, from ui.cpp's intake. Each publishes Topic::Media. ----
void media_take_track(const char *source, const char *title, const char *artist, const char *state,
                      bool playing, bool controllable);
void media_take_cover(const void *pixels, bool placeholder);
void media_take_large_cover(const void *pixels);
void media_take_progress(int position_s, int duration_s, bool playing);
void media_take_volume(int percent);
void media_take_remote(bool remote);
void media_take_video(bool seeks);
void media_take_segments(const MediaSegment *segments, int count);
void media_take_subtitles(bool available, bool shown);
void media_take_still(const void *pixels);
void media_take_neighbours(bool before, bool after);
void media_take_hold_preset(int preset);

// ---- What follows from it. ----
/** Seconds, and milliseconds, into what plays, carried on while it plays. */
int  media_position_now();
int  media_position_ms_now();
bool media_is_video();

/** What the skip button offers now: into the intro, its end; into the credits,
 *  or near the end without any marked, the next episode, if there is one. */
struct MediaSkip {
    const char *text = nullptr;  // null when there is nothing to skip
    int         to_s = -1;       // where skipping the intro seeks to
    bool        next = false;    // skipping starts the next episode instead
};
MediaSkip media_skip_offer();

// ---- Commands, from the views. ----
/** A player that takes no commands is followed, not steered: only the volume goes to it. */
bool media_steers(MediaAction action);
void media_action(MediaAction action);
void media_toggle_play();
void media_seek_to(int position_s);
void media_seek_by(int delta_s);
void media_skip();
/** Turned at once, as the player will have them by its next report. */
void media_toggle_subtitles();
/** Set from here, as a slider is dragged: shown at once and sent on. */
void media_set_volume(int percent);

// ---- The favourites, held while nothing plays: Topic::Picks. ----
struct Pick {
    char          name[48] = "";  // empty leaves it out
    const void   *art      = nullptr;  // media::kPickArtSize square, RGB565, or null
    std::uint32_t arts     = 0;        // counts every cover that came for it
};
const Pick &media_pick(int index);
int         media_pick_count();  // the named ones
void        media_take_pick(int index, const char *name);
void        media_take_pick_art(int index, const void *pixels);

}  // namespace ui::detail
