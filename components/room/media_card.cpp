#include "players.h"
#include "room.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "ha_ws.h"
#include "media.h"
#include "room_layout.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <span>
#include <utility>

// The media card, the music view and the cinema: whichever player is chosen,
// drawn from its view alone, and every button handed to it.
namespace room {
namespace {
constexpr char TAG[] = "media";

constexpr float VOLUME_STEP = 0.05f;  // of full scale, per press, where there is a level

// What was set stands until the player reports it, or this long: a player
// reports slowly and late, and a drag's earlier levels came back after it.
constexpr std::int64_t VOLUME_HOLD_US    = 10 * units::kUsPerSecond;
constexpr int          VOLUME_AGREES     = 1;  // percent either way, for rounding
constexpr std::int64_t MEDIA_GONE_US     = 3 * units::kUsPerSecond;
constexpr std::int64_t VOLUME_MIN_GAP_US = 250 * units::kUsPerMs;

// The sources tell their players from tasks of their own, one at a time.
std::mutex            s_lock;
std::atomic<Player *> s_on{nullptr};  // what the card shows, and so steers
std::atomic<bool>     s_muted{false};
std::atomic<bool>     s_steps{false};
std::atomic<int>      s_volume_pct{-1};  // negative until the player reports one

std::atomic<int>          s_volume_pending{-1};
std::atomic<std::int64_t> s_volume_sent_us{0};
esp_timer_handle_t        s_volume_timer = nullptr;
std::atomic<std::int64_t> s_volume_set_us{0};  // 0 once the player agreed
std::atomic<int>          s_volume_set{-1};

std::string s_position_stamp;
int         s_position_duration = -1;
bool        s_position_playing  = false;

Player &steered()
{
    Player *player = s_on.load(std::memory_order_relaxed);
    return player != nullptr ? *player : speaker_player();
}

/** The player the card shows and steers: anything playing before anything
 *  paused, Jellyfin before the speaker before the laptop; the speaker while
 *  none has anything. Among the paused the one on the card stays: a pause from
 *  here must not hand the card, and the next tap, to another paused player. */
Player &choose(PlayerView &view)
{
    Player *const order[] = {&jellyfin_player(), &speaker_player(), &laptop_player()};
    Player *const shown   = &steered();
    Player *const paused[] = {shown, order[0], order[1], order[2]};
    for (const bool want_playing : {true, false}) {
        for (Player *player : want_playing ? std::span<Player *const>(order) : std::span<Player *const>(paused)) {
            PlayerView candidate = player->view();
            if (candidate.going() && (candidate.state == "paused") != want_playing) {
                view = std::move(candidate);
                return *player;
            }
        }
    }
    view = speaker_player().view();
    return speaker_player();
}

/** A player that vanishes or loses its title for a moment is not shown as
 *  gone until it has stayed so for a while. */
bool gone_only_briefly(const PlayerView &view)
{
    const bool bare = view.known && view.state != "off" && view.state != "idle" && view.title.empty();

    static std::int64_t s_gone_us = 0;
    if (view.known && !bare) {
        s_gone_us = 0;
        return false;
    }
    const std::int64_t now = esp_timer_get_time();
    if (s_gone_us == 0) {
        s_gone_us = now;
    }
    return now - s_gone_us < MEDIA_GONE_US;
}

void show_text(const std::string &source, const std::string &title, const std::string &artist,
               const std::string &state, bool playing, bool art_coming)
{
    static std::string s_shown;
    const std::string  shown = source + '\n' + title + '\n' + artist + '\n' + state + (playing ? "\n1" : "\n0");
    if (shown != s_shown) {
        s_shown = shown;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media(source.c_str(), title.c_str(), artist.c_str(), state.c_str(),
                                                    playing, state != "OFF" && state != "--", art_coming));
    }
}

// A new track comes with the last one's position until the speaker reports
// afresh, seconds later: until then it is taken as at its start, so the times
// change with the title rather than after it.
void show_position(const PlayerView &view, bool playing)
{
    static std::string s_track;
    static std::string s_seen_key;   // the position's stamp the last time round
    static std::string s_stale_key;  // the last track's, while the new one still has it
    const std::string  track = view.title.empty() ? "" : view.title + '\n' + view.artist;
    if (track != s_track) {
        s_stale_key = s_track.empty() || track.empty() ? "" : s_seen_key;
        s_track     = track;
    }
    s_seen_key              = view.position_key;
    const bool        stale = !s_stale_key.empty() && view.position_key == s_stale_key;
    const std::string stamp = stale ? "start of " + track : view.position_key;
    if (stamp == s_position_stamp && view.duration_s == s_position_duration && playing == s_position_playing) {
        return;
    }
    s_position_stamp    = stamp;
    s_position_duration = view.duration_s;
    s_position_playing  = playing;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_progress(stale ? 0 : view.position_s, view.duration_s, playing));
}

void show_volume(const PlayerView &view, bool moved)
{
    if (view.volume < 0.0f) {
        if (moved) {
            s_volume_pct.store(-1, std::memory_order_relaxed);
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(-1));
        }
        return;
    }
    const int          percent = percent_of(view.volume);
    const std::int64_t set_at  = s_volume_set_us.load(std::memory_order_relaxed);
    if (set_at != 0) {
        const bool agrees = std::abs(percent - s_volume_set.load(std::memory_order_relaxed)) <= VOLUME_AGREES;
        if (!agrees && esp_timer_get_time() - set_at < VOLUME_HOLD_US) {
            return;
        }
        s_volume_set_us.store(0, std::memory_order_relaxed);
    }
    s_volume_pct.store(percent, std::memory_order_relaxed);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(percent));
}

/** Asks for the track's cover once it changed, or once a new one came for
 *  it, as a laptop's does when the browser gives one late or a sharper one is
 *  found; true when a different one is now on its way, which the title waits for. */
bool ask_for_art(const std::string &picture, const std::string &title)
{
    static std::string s_art_title;
    static std::string s_art_path;
    static bool        s_art_asked = false;
    if (title != s_art_title || (s_art_asked && !picture.empty() && picture != s_art_path)) {
        s_art_title = title;
        s_art_asked = false;
    }
    if (s_art_asked || (picture.empty() && !title.empty())) {
        return false;
    }
    s_art_asked            = true;
    const std::string path = title.empty() ? "" : picture;
    const bool        other = !path.empty() && path != s_art_path;
    s_art_path             = path;
    media::set_art_path(path.c_str());
    return other;
}

bool same_segments(const std::vector<ui::MediaSegment> &a, const std::vector<ui::MediaSegment> &b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const ui::MediaSegment &x, const ui::MediaSegment &y) {
        return x.kind == y.kind && x.start_s == y.start_s && x.end_s == y.end_s;
    });
}

// What the player takes, said to the screen only when it changes.
void show_takes(const PlayerView &view)
{
    static int s_takes = -1;
    const int  takes   = (view.remote ? 1 : 0) | (view.tracks_back ? 2 : 0) | (view.tracks_on ? 4 : 0) |
                      (view.takes_volume ? 8 : 0) | (view.takes_subtitles ? 16 : 0) | (view.steps_volume ? 32 : 0) |
                      (view.pause_settles ? 64 : 0) | (view.kind == Kind::Video ? 128 : 0);
    if (std::exchange(s_takes, takes) == takes) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_remote(view.remote));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_tracks(view.tracks_back, view.tracks_on));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_takes(view.takes_volume, view.takes_subtitles, view.steps_volume));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_pause_settles(view.pause_settles));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_seeks(view.kind == Kind::Video));
}

// A video's extras: the episodes either side, its intro and credits, its subtitles.
void show_extras(const PlayerView &view)
{
    static int s_around = -1;
    const int  around   = (view.before ? 1 : 0) | (view.after ? 2 : 0) | (view.episodes ? 4 : 0);
    if (std::exchange(s_around, around) != around) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_neighbours(view.before, view.after, view.episodes));
    }
    static std::vector<ui::MediaSegment> s_segments;
    static bool                          s_segments_told = false;
    if (!s_segments_told || !same_segments(s_segments, view.segments)) {
        s_segments      = view.segments;
        s_segments_told = true;
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_media_segments(view.segments.data(), static_cast<int>(view.segments.size())));
    }
    static int s_subtitles = -1;
    const int  subtitles   = (view.subtitles_available ? 1 : 0) | (view.subtitles_shown ? 2 : 0);
    if (std::exchange(s_subtitles, subtitles) != subtitles) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_subtitles(view.subtitles_available, view.subtitles_shown));
    }
    media::set_still_url(view.kind == Kind::Video ? view.still.c_str() : "");
}

/** With the lock held. */
void show()
{
    PlayerView    view;
    Player       &player = choose(view);
    Player *const before = s_on.exchange(&player);
    const bool    moved  = before != &player;
    static int    s_hold = -2;
    if (std::exchange(s_hold, view.hold_preset) != view.hold_preset) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_hold_preset(view.hold_preset));
    }
    for (Player *each : {&jellyfin_player(), &speaker_player(), &laptop_player()}) {
        each->shown(each == &player);
    }
    s_steps.store(view.steps_volume, std::memory_order_relaxed);
    show_takes(view);
    show_extras(view);
    if (gone_only_briefly(view)) {
        return;
    }
    if (!view.known) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("SPEAKER", "", "", "--", false, false));
        media::set_art_path("");
        return;
    }
    const bool playing = view.state == "playing";
    // What an idle player still names is what it last played: nothing is on.
    const bool        on    = view.going();
    const std::string title = on ? view.title : "";
    // An idle speaker has nothing on: to look at, it is off.
    const std::string state = view.state == "idle" ? "OFF" : upper(view.state);
    s_muted.store(view.muted, std::memory_order_relaxed);
    const bool art_coming = ask_for_art(view.picture, title);
    show_text(view.source, title, on ? view.artist : "", state, playing, art_coming);
    show_position(view, playing);
    show_volume(view, moved);
}

void send_volume(void *)
{
    const int percent = s_volume_pending.exchange(-1, std::memory_order_relaxed);
    if (percent < 0) {
        return;
    }
    s_volume_sent_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    steered().set_volume(percent);
}

void nudge_volume(float delta)
{
    const int reported = s_volume_pct.load(std::memory_order_relaxed);
    if (reported < 0) {
        return;  // a blind guess would jump the volume
    }
    float wanted = static_cast<float>(reported) / kPercentPerWhole + delta;
    wanted       = std::clamp(wanted, 0.0f, 1.0f);
    on_media_volume(percent_of(wanted));
}
}  // namespace

void refresh_media()
{
    std::lock_guard<std::mutex> hold(s_lock);
    show();
}

void media_init()
{
    const esp_timer_create_args_t volume_timer = {
        .callback              = send_volume,
        .arg                   = nullptr,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "volume",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&volume_timer, &s_volume_timer));
}

void media_from_store(const hass::ws::EntityStore &store)
{
    std::lock_guard<std::mutex> hold(s_lock);
    take_speaker(store.find(MEDIA_SPEAKER));
    show();
}

void on_jellyfin(const jellyfin::NowPlaying &now)
{
    std::lock_guard<std::mutex> hold(s_lock);
    take_jellyfin(now);
    show();
}

void on_laptop(const laptop::NowPlaying &now)
{
    std::lock_guard<std::mutex> hold(s_lock);
    take_laptop(now);
    show();
}

void on_media_volume(int percent)
{
    percent = std::clamp(percent, 0, static_cast<int>(kPercentPerWhole));
    ESP_LOGI(TAG, "volume %d%%", percent);

    s_volume_pct.store(percent, std::memory_order_relaxed);
    s_volume_set.store(percent, std::memory_order_relaxed);
    s_volume_set_us.store(esp_timer_get_time(), std::memory_order_relaxed);

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(percent));

    s_volume_pending.store(percent, std::memory_order_relaxed);

    const std::int64_t now   = esp_timer_get_time();
    const std::int64_t since = now - s_volume_sent_us.load(std::memory_order_relaxed);
    if (since >= VOLUME_MIN_GAP_US) {
        send_volume(nullptr);
        return;
    }
    if (s_volume_timer != nullptr) {
        esp_timer_stop(s_volume_timer);  // not running yet is not an error worth reporting
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_start_once(s_volume_timer, VOLUME_MIN_GAP_US - since));
    }
}

void on_seek(int position_s)
{
    steered().seek(position_s);
}

void on_pick(int index)
{
    play_pick(index);
}

void on_media(ui::MediaAction action)
{
    Player &player = steered();
    switch (action) {
        case ui::MediaAction::PlayPause:
            player.play_pause();
            break;
        case ui::MediaAction::Previous:
        case ui::MediaAction::Next:
            player.skip(action == ui::MediaAction::Next);
            break;
        case ui::MediaAction::VolumeDown:
        case ui::MediaAction::VolumeUp: {
            const bool up = action == ui::MediaAction::VolumeUp;
            if (s_steps.load(std::memory_order_relaxed)) {
                player.step_volume(up);
            } else {
                nudge_volume(up ? VOLUME_STEP : -VOLUME_STEP);
            }
            break;
        }
        case ui::MediaAction::Mute:
            player.set_muted(!s_muted.load(std::memory_order_relaxed));
            break;
        case ui::MediaAction::Subtitles:
            player.toggle_subtitles();
            break;
    }
}
}  // namespace room
