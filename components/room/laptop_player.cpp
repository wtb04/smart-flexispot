#include "players.h"

#include "laptop.h"

// A laptop's Now Playing, through Desk Link: a video or music, whichever it
// says, and the volume of its own output where it has one.
namespace room {
namespace {
class Laptop : public Player {
public:
    void take(const laptop::NowPlaying &now)
    {
        PlayerView view;
        view.steps_volume    = now.takes_volume_step && !now.takes_volume;
        view.takes_volume    = now.takes_volume || view.steps_volume;
        view.takes_subtitles = false;
        view.volume = now.takes_volume && now.volume >= 0 ? now.volume / kPercentPerWhole : kNoNumber;
        view.muted  = now.muted;
        if (now.active) {
            view.known        = true;
            view.kind         = now.video ? Kind::Video : Kind::Audio;
            view.state        = now.playing ? "playing" : "paused";
            view.source       = now.app.empty() ? "LAPTOP" : upper(now.app) + ", LAPTOP";
            view.title        = now.title;
            view.artist       = now.artist;
            // A video's channel picture on the card, where its frame would be cropped.
            view.picture      = laptop::art_url(now.square.empty() ? now.art : now.square);
            view.still        = now.video ? laptop::art_url(now.art) : "";
            view.position_s   = now.position_s;
            view.duration_s   = now.duration_s;
            view.position_key = now.title + ':' + std::to_string(now.position_s) + (now.playing ? "" : "p");
            view.remote       = now.takes_pause;
            view.tracks_back  = now.takes_previous;
            view.tracks_on    = now.takes_next;
            view.before       = now.video && now.takes_previous;
            view.after        = now.video && now.takes_next;
        }
        view_ = view;
    }

    PlayerView view() const override { return view_; }

    void play_pause() override { laptop::play_pause(); }
    void seek(int position_s) override { laptop::seek(position_s); }
    void skip(bool next) override { next ? laptop::next() : laptop::previous(); }
    void set_volume(int percent) override { laptop::set_volume(percent); }
    void step_volume(bool up) override { laptop::step_volume(up); }
    void set_muted(bool muted) override { laptop::set_muted(muted); }

private:
    PlayerView view_;
};

Laptop s_laptop;
}  // namespace

Player &laptop_player()
{
    return s_laptop;
}

void take_laptop(const laptop::NowPlaying &now)
{
    s_laptop.take(now);
}
}  // namespace room
