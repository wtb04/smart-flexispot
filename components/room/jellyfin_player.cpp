#include "players.h"

#include "esp_log.h"
#include "jellyfin.h"
#include "jobs.h"
#include "segments.h"

#include <mutex>

// Jellyfin's followed session: a film or an episode, its still, and for an
// episode its intro and credits and the episodes either side, looked up
// while it is on the card.
namespace room {
namespace {
constexpr char TAG[] = "jellyfin";

constexpr int JELLYFIN_COVER_H = 300;  // enough for the card's frame, square from the middle
constexpr int JELLYFIN_PRESET  = 1;    // holding the card for Jellyfin: Preset 2

// Asked of Jellyfin on a task of its own when an episode comes on the card: a
// slow server must not hold up the socket the page is drawn from.
struct Extras {
    std::string                   item;  // the episode they are of
    jellyfin::Neighbours          around;
    std::vector<ui::MediaSegment> segments;
};

std::mutex  s_extras_lock;
std::string s_wanted;  // the episode to look up, empty for none
std::string s_wanted_series;
Extras      s_extras;
jobs::Job   s_job = jobs::kNoJob;

jobs::Result look_up()
{
    Extras      found;
    std::string series;
    {
        std::lock_guard<std::mutex> hold(s_extras_lock);
        found.item = s_wanted;
        series     = s_wanted_series;
    }
    std::string answer;
    if (!found.item.empty() && !series.empty() &&
        jellyfin::fetch(jellyfin::neighbours_path(series, found.item), answer)) {
        found.around = jellyfin::neighbours(answer, found.item);
    }
    if (!found.item.empty() && jellyfin::fetch(segments::path_for(found.item), answer)) {
        for (const segments::Segment &segment : segments::parse(answer)) {
            if (found.segments.size() < ui::kMaxSegments) {
                found.segments.push_back({segment.kind == segments::Kind::Intro ? ui::MediaSegment::Kind::Intro
                                                                                : ui::MediaSegment::Kind::Credits,
                                          segment.start_s, segment.end_s});
            }
        }
        ESP_LOGI(TAG, "%u segments for the episode", static_cast<unsigned>(found.segments.size()));
    }
    {
        std::lock_guard<std::mutex> hold(s_extras_lock);
        if (found.item != s_wanted) {
            return jobs::sleep();  // another episode came on meanwhile, and is looked up next
        }
        s_extras = std::move(found);
    }
    refresh_media();
    return jobs::sleep();
}

void want(const std::string &episode, const std::string &series)
{
    {
        std::lock_guard<std::mutex> hold(s_extras_lock);
        if (episode == s_wanted) {
            return;
        }
        s_wanted        = episode;
        s_wanted_series = series;
    }
    if (s_job == jobs::kNoJob) {
        jobs::Spec spec;
        spec.name     = "segments";
        spec.lane     = jobs::Lane::Slow;
        spec.first_ms = jobs::kNever;
        spec.run      = look_up;
        s_job         = jobs::add(std::move(spec));
    }
    jobs::poke(s_job);
}

class Jellyfin : public Player {
public:
    void take(const jellyfin::NowPlaying &now)
    {
        PlayerView view;
        view.kind            = Kind::Video;
        view.hold_preset     = JELLYFIN_PRESET;
        view.takes_volume    = now.takes_volume;
        view.takes_subtitles = now.takes_subtitles;
        episode_             = "";
        series_              = "";
        if (now.active) {
            view.known  = true;
            view.state  = now.paused ? "paused" : "playing";
            view.source = "JELLYFIN";
            view.title  = now.title;
            view.artist = episode_line(now.series, now.season, now.episode);
            // An episode's own picture is a still from it: the card shows its
            // season's poster, or its series', and the cinema the still.
            const std::string &poster = !now.season_id.empty() ? now.season_id
                                      : !now.series_id.empty() ? now.series_id
                                                               : now.item;
            view.picture      = jellyfin::cover_url(now.kind == "Episode" ? poster : now.item, JELLYFIN_COVER_H);
            view.still        = jellyfin::cover_url(now.item, JELLYFIN_COVER_H);
            view.position_s   = now.position_s;
            view.duration_s   = now.duration_s;
            view.position_key = now.item + ':' + std::to_string(now.position_s) + (now.paused ? "p" : "");
            view.volume       = now.volume >= 0 && now.takes_volume ? now.volume / kPercentPerWhole : kNoNumber;
            view.remote       = now.remote;
            view.subtitles_available = now.takes_subtitles && now.subtitle_track >= 0;
            view.subtitles_shown     = now.subtitle >= 0;
            episode_                 = now.kind == "Episode" ? now.item : "";
            series_                  = now.series_id;
        }
        view_ = view;
    }

    PlayerView view() const override
    {
        PlayerView view = view_;
        std::lock_guard<std::mutex> hold(s_extras_lock);
        if (!episode_.empty() && s_extras.item == episode_) {
            view.before   = !s_extras.around.previous.empty();
            view.after    = !s_extras.around.next.empty();
            view.segments = s_extras.segments;
        }
        return view;
    }

    void play_pause() override { jellyfin::play_pause(); }
    void seek(int position_s) override { jellyfin::seek(position_s); }
    void set_volume(int percent) override { jellyfin::set_volume(percent); }
    void toggle_subtitles() override { jellyfin::toggle_subtitles(); }

    void skip(bool next) override
    {
        std::string item;
        {
            // What is on the card, as shown() said; not the episode itself,
            // which Jellyfin's task changes under the card's lock.
            std::lock_guard<std::mutex> hold(s_extras_lock);
            if (!s_wanted.empty() && s_extras.item == s_wanted) {
                item = next ? s_extras.around.next : s_extras.around.previous;
            }
        }
        jellyfin::play_now(item);
    }

    void shown(bool on_card) override { want(on_card ? episode_ : "", series_); }

private:
    PlayerView  view_;
    std::string episode_;  // the episode it plays, empty for a film or nothing
    std::string series_;
};

Jellyfin s_jellyfin;
}  // namespace

Player &jellyfin_player()
{
    return s_jellyfin;
}

void take_jellyfin(const jellyfin::NowPlaying &now)
{
    s_jellyfin.take(now);
}
}  // namespace room
