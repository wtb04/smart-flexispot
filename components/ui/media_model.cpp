#include "media_model.h"

#include "topics.h"
#include "ui_internal.h"

#include "freertos/task.h"
#include "units.h"

#include <algorithm>
#include <cstdio>

namespace ui::detail {
namespace {
constexpr int NEXT_UP_TAIL_S = 30;  // with no credits marked, the end is near
// Between one episode and the next, Jellyfin says for a moment that nothing
// plays: a video that goes is shown gone only once it has stayed gone this
// long. Music goes straight on to its next track, so a stop shows at once.
constexpr std::uint32_t GONE_SETTLE_MS = 4000;

MediaState  s_media;
Pick        s_picks[media::kPickCount];
lv_timer_t *s_pause_timer = nullptr;

// What it said while the last track seemed to go, kept until that has lasted.
// Its pictures too: the cover and the still going blank are held as its text
// is, so the card and the cinema view keep the episode's through the gap.
struct Gone {
    lv_timer_t *timer = nullptr;
    char        source[sizeof(MediaState::source)] = "";
    char        state[sizeof(MediaState::state)]   = "";
    bool        controllable = false;
    bool        video_told   = false;  // the player stopped seeking meanwhile, or started again
    bool        video        = false;
    bool        cover_told   = false;  // the cover went blank meanwhile
    bool        placeholder  = false;
    int         art_width    = media::kArtSize;
    bool        still_told   = false;  // and the still
} s_gone;

void copy(char *to, std::size_t size, const char *from)
{
    std::snprintf(to, size, "%s", from != nullptr ? from : "");
}

void cancel_pause_settle()
{
    if (s_pause_timer != nullptr) {
        lv_timer_delete(s_pause_timer);
        s_pause_timer = nullptr;
    }
}

void show_playing(bool playing)
{
    s_media.playing = playing;
    publish(Topic::Media);
}

// A pause from the player shows only once it has lasted: one between tracks,
// or a moment's stall, is not shown at all.
void pause_settled(lv_timer_t *)
{
    cancel_pause_settle();
    show_playing(false);
}
}  // namespace

const MediaState &media_state()
{
    return s_media;
}

namespace {
void apply_gone(lv_timer_t *);

void settle_gone()
{
    if (s_gone.timer != nullptr) {
        lv_timer_delete(s_gone.timer);
        s_gone.timer = nullptr;
    }
}

void media_apply_track(const char *source, const char *title, const char *artist, const char *state,
                       bool playing, bool controllable);

// It stayed gone: shown so now, as it was told.
void apply_gone(lv_timer_t *)
{
    settle_gone();
    if (s_gone.video_told) {
        s_media.video = s_gone.video;
    }
    if (s_gone.cover_told) {
        s_media.art         = nullptr;
        s_media.large       = nullptr;
        s_media.art_width   = s_gone.art_width;
        s_media.placeholder = s_gone.placeholder;
        ++s_media.covers;
    }
    if (s_gone.still_told) {
        s_media.still = nullptr;
        ++s_media.stills;
    }
    media_apply_track(s_gone.source, nullptr, nullptr, s_gone.state, false, s_gone.controllable);
}
}  // namespace

void media_take_track(const char *source, const char *title, const char *artist, const char *state,
                      bool playing, bool controllable)
{
    const bool has_track = title != nullptr && title[0] != '\0';
    if (!has_track && s_media.has_track && s_media.video) {
        copy(s_gone.source, sizeof(s_gone.source), source);
        copy(s_gone.state, sizeof(s_gone.state), state);
        s_gone.controllable = controllable;
        if (s_gone.timer == nullptr) {
            s_gone.video_told = false;
            s_gone.cover_told = false;
            s_gone.still_told = false;
            s_gone.timer      = lv_timer_create(apply_gone, GONE_SETTLE_MS, nullptr);
            lv_timer_set_repeat_count(s_gone.timer, 1);
            lv_timer_set_auto_delete(s_gone.timer, false);
        }
        return;
    }
    if (s_gone.timer != nullptr) {
        settle_gone();  // the next one came in time: on to it, as if nothing had gone
        if (s_gone.video_told) {
            s_media.video = s_gone.video;
        }
    }
    media_apply_track(source, title, artist, state, playing, controllable);
}

namespace {
void media_apply_track(const char *source, const char *title, const char *artist, const char *state,
                       bool playing, bool controllable)
{
    const bool has_track = title != nullptr && title[0] != '\0';
    copy(s_media.source, sizeof(s_media.source), source != nullptr && source[0] != '\0' ? source : "SPEAKER");
    copy(s_media.title, sizeof(s_media.title), has_track ? title : "");
    copy(s_media.artist, sizeof(s_media.artist), has_track ? artist : "");
    copy(s_media.state, sizeof(s_media.state), state != nullptr ? state : "--");
    s_media.has_track    = has_track;
    s_media.controllable = controllable;
    if (playing || !has_track || !s_media.pause_settles) {
        cancel_pause_settle();
        s_media.playing = playing;
    } else if (s_media.playing && s_pause_timer == nullptr) {
        s_pause_timer = lv_timer_create(pause_settled, PAUSE_SETTLE_MS, nullptr);
    }
    publish(Topic::Media);
}
}  // namespace

void media_take_cover(const void *pixels, bool placeholder, int width)
{
    if (pixels == nullptr && s_gone.timer != nullptr) {
        s_gone.cover_told  = true;  // the old one's buffer stays as it is until the next cover
        s_gone.placeholder = placeholder;
        s_gone.art_width   = width;
        return;
    }
    s_media.art         = pixels;
    s_media.art_width   = width;
    s_media.placeholder = placeholder;
    s_media.large       = nullptr;  // the old record's, until this one's large one follows
    ++s_media.covers;
    publish(Topic::Media);
}

void media_take_large_cover(const void *pixels)
{
    if (pixels == nullptr && s_gone.timer != nullptr) {
        return;  // goes with the cover, as apply_gone clears both
    }
    s_media.large = pixels;
    ++s_media.covers;
    publish(Topic::Media);
}

void media_take_progress(int position_s, int duration_s, bool playing)
{
    s_media.position_s  = position_s;
    s_media.duration_s  = duration_s;
    s_media.advancing   = playing;
    s_media.position_at = xTaskGetTickCount();
    publish(Topic::Media);
}

void media_take_volume(int percent)
{
    s_media.volume = percent;
    publish(Topic::Media);
}

void media_take_remote(bool remote)
{
    s_media.remote = remote;
    publish(Topic::Media);
}

void media_take_pause_settles(bool settles)
{
    s_media.pause_settles = settles;
}

void media_take_takes(bool volume, bool subtitles)
{
    s_media.takes_volume    = volume;
    s_media.takes_subtitles = subtitles;
    publish(Topic::Media);
}

void media_take_video(bool seeks)
{
    if (s_gone.timer != nullptr) {
        s_gone.video_told = true;  // kept with what went, and given as it goes or comes back
        s_gone.video      = seeks;
        return;
    }
    s_media.video = seeks;
    publish(Topic::Media);
}

void media_take_segments(const MediaSegment *segments, int count)
{
    s_media.segment_count = std::clamp(count, 0, kMaxSegments);
    std::copy(segments, segments + s_media.segment_count, s_media.segments);
    publish(Topic::Media);
}

void media_take_subtitles(bool available, bool shown)
{
    s_media.subtitles_available = available;
    s_media.subtitles_shown     = shown;
    publish(Topic::Media);
}

void media_take_still(const void *pixels)
{
    if (pixels == nullptr && s_gone.timer != nullptr) {
        s_gone.still_told = true;
        return;
    }
    s_media.still = pixels;
    ++s_media.stills;
    publish(Topic::Media);
}

void media_take_neighbours(bool before, bool after)
{
    s_media.before = before;
    s_media.after  = after;
    publish(Topic::Media);
}

void media_take_tracks(bool back, bool on)
{
    s_media.tracks_back = back;
    s_media.tracks_on   = on;
    publish(Topic::Media);
}

void media_take_hold_preset(int preset)
{
    s_media.hold_preset = preset;
    publish(Topic::Media);
}

int media_position_now()
{
    return media_position_ms_now() / units::kMsPerSecond;
}

int media_position_ms_now()
{
    std::int64_t at = static_cast<std::int64_t>(s_media.position_s) * units::kMsPerSecond;
    if (s_media.advancing) {
        at += static_cast<std::int64_t>(xTaskGetTickCount() - s_media.position_at) * units::kMsPerSecond /
              configTICK_RATE_HZ;
    }
    const std::int64_t end = static_cast<std::int64_t>(s_media.duration_s) * units::kMsPerSecond;
    return static_cast<int>(end > 0 ? std::min(at, end) : at);
}

bool media_is_video()
{
    return s_media.video && s_media.has_track;
}

bool media_shows_volume()
{
    return s_media.takes_volume && (s_media.remote || s_media.volume >= 0);
}

bool media_shows_subtitles()
{
    return s_media.takes_subtitles && (s_media.remote || s_media.subtitles_available);
}

MediaSkip media_skip_offer()
{
    MediaSkip offer;
    if (!s_media.remote || !media_is_video() || s_media.duration_s <= 0) {
        return offer;
    }
    const int at            = media_position_now();
    bool      credits_known = false;
    bool      next          = false;
    for (int i = 0; i < s_media.segment_count; ++i) {
        const MediaSegment &segment = s_media.segments[i];
        const bool          intro   = segment.kind == MediaSegment::Kind::Intro;
        credits_known               = credits_known || !intro;
        if (at >= segment.start_s && at < segment.end_s) {
            if (intro) {
                offer.text = "Skip intro";
                offer.to_s = segment.end_s;
            } else {
                next = true;
            }
        }
    }
    next = next || (!credits_known && s_media.duration_s > NEXT_UP_TAIL_S && at >= s_media.duration_s - NEXT_UP_TAIL_S);
    // Seeking to the end only stops the player there; the episode after is
    // started instead, and without one there is nothing to go on to.
    if (next && s_media.after) {
        offer.text = "Next episode";
        offer.next = true;
    }
    return offer;
}

bool media_steers(MediaAction action)
{
    return s_media.remote || action == MediaAction::VolumeDown || action == MediaAction::VolumeUp ||
           action == MediaAction::Mute || action == MediaAction::Subtitles;
}

void media_action(MediaAction action)
{
    if (media_steers(action) && s_handlers.media != nullptr) {
        s_handlers.media(action);
    }
}

void media_toggle_play()
{
    if (!media_steers(MediaAction::PlayPause)) {
        return;
    }
    cancel_pause_settle();
    show_playing(!s_media.playing);  // at once, as the player will be by the time it says so
    if (s_handlers.media != nullptr) {
        s_handlers.media(MediaAction::PlayPause);
    }
}

void media_seek_to(int position_s)
{
    if (!s_media.remote) {
        return;
    }
    position_s = std::max(0, s_media.duration_s > 0 ? std::min(position_s, s_media.duration_s) : position_s);
    s_media.position_s  = position_s;
    s_media.position_at = xTaskGetTickCount();
    publish(Topic::Media);
    if (s_handlers.seek != nullptr) {
        s_handlers.seek(position_s);
    }
}

void media_seek_by(int delta_s)
{
    media_seek_to(media_position_now() + delta_s);
}

void media_skip()
{
    const MediaSkip offer = media_skip_offer();
    if (offer.next) {
        media_action(MediaAction::Next);
    } else if (offer.to_s >= 0) {
        media_seek_to(offer.to_s);
    }
}

void media_toggle_subtitles()
{
    s_media.subtitles_shown = !s_media.subtitles_shown;
    publish(Topic::Media);
    media_action(MediaAction::Subtitles);
}

void media_set_volume(int percent)
{
    if (percent == s_media.volume) {
        return;
    }
    media_take_volume(percent);
    if (s_handlers.media_volume != nullptr) {
        s_handlers.media_volume(percent);
    }
}

const Pick &media_pick(int index)
{
    return s_picks[index];
}

int media_pick_count()
{
    return static_cast<int>(std::count_if(std::begin(s_picks), std::end(s_picks),
                                          [](const Pick &pick) { return pick.name[0] != '\0'; }));
}

void media_take_pick(int index, const char *name)
{
    copy(s_picks[index].name, sizeof(s_picks[index].name), name);
    publish(Topic::Picks);
}

void media_take_pick_art(int index, const void *pixels)
{
    s_picks[index].art = pixels;
    ++s_picks[index].arts;
    publish(Topic::Picks);
}

}  // namespace ui::detail
