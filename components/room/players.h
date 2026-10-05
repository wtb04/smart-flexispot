#pragma once

#include "ha_values.h"
#include "ha_ws_protocol.h"
#include "jellyfin_protocol.h"
#include "laptop_protocol.h"
#include "ui.h"

#include <string>
#include <vector>

// The players the media card follows, each behind the same face: what it
// plays and what it takes, as a PlayerView, and the card's buttons carried
// out its own way. The card itself, media_card.cpp, draws whichever is chosen
// from its view alone and knows none of them.
namespace room {
enum class Kind : std::uint8_t { Audio, Video };  // the music view and card, or the cinema

struct PlayerView {
    bool        known = false;  // there is a player to speak of, playing or not
    Kind        kind  = Kind::Audio;
    std::string state;          // playing, paused, buffering, idle, off
    std::string source;         // what the card calls it
    std::string title;
    std::string artist;         // for a video, the series, a newline, and its season and episode
    std::string picture;        // the cover, a path on Home Assistant or a whole address
    std::string still;          // a video's own picture, for the cinema
    std::string position_key;   // changes whenever the position is reported afresh
    int         position_s = 0;
    int         duration_s = 0;
    float       volume     = kNoNumber;
    bool        muted      = false;

    // What it takes. A control it never takes is left out, not faded.
    bool remote          = true;   // play, pause and seek; a player that is only followed takes none
    bool tracks_back     = true;   // a track back, and on: a radio station takes neither
    bool tracks_on       = true;
    bool takes_volume    = true;
    bool steps_volume    = false;  // up and down only, with no level to show
    bool takes_subtitles = false;

    // A video's extras, where its player knows them.
    bool                          subtitles_available = false;
    bool                          subtitles_shown     = false;
    bool                          before = false;  // the cinema's previous and next
    bool                          after  = false;
    std::vector<ui::MediaSegment> segments;        // its intro and credits, to skip

    bool pause_settles = false;  // a pause is only shown once it has lasted, as a speaker says paused between songs
    int  hold_preset   = -1;     // the desk preset holding the card goes to; -1 for the music view

    bool going() const { return known && (state == "playing" || state == "paused" || state == "buffering"); }
};

class Player {
public:
    virtual ~Player() = default;

    /** Its view now; with the card's lock held. */
    virtual PlayerView view() const = 0;

    virtual void play_pause()           = 0;
    virtual void seek(int position_s)   = 0;
    virtual void skip(bool next)        = 0;  // a track, or an episode
    virtual void set_volume(int percent) {}
    virtual void step_volume(bool up) {}
    virtual void set_muted(bool muted) {}
    virtual void toggle_subtitles() {}

    /** Whether it is on the card now; one with more to look up, as Jellyfin's
     *  intro and credits, asks for it only then. With the card's lock held. */
    virtual void shown(bool on_card) {}
};

// The players, each in a file of its own. A player that learns more on its
// own, as Jellyfin its intro and credits, asks the card to draw again.
Player &speaker_player();
Player &jellyfin_player();
Player &laptop_player();

/** What each player is told of its source, with the card's lock held. */
void take_speaker(const hass::ws::Entity *speaker);
void take_jellyfin(const jellyfin::NowPlaying &now);
void take_laptop(const laptop::NowPlaying &now);

/** The favourites the card offers, which play on the speaker. */
void start_picks();
void play_pick(int index);

/** Draws the card again from the chosen player's view; takes the card's lock. */
void refresh_media();

/** The card's part of room::init() and room::render(). */
void media_init();
void media_from_store(const hass::ws::EntityStore &store);
}  // namespace room
