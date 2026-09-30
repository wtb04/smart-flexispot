#include "room.h"

#include "clock_math.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ha_ws.h"
#include "jellyfin.h"
#include "media.h"
#include "esp_heap_caps.h"
#include "net.h"
#include "jobs.h"
#include "picks.h"
#include "room_layout.h"
#include "segments.h"
#include "radar.h"
#include "ui.h"
#include "units.h"


#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <ctime>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace room {
namespace {
constexpr char TAG[] = "room";

constexpr char CLIMATE_ENTITY[] = "climate.office_thermostaat";

// Two players, one card: the speaker, then Jellyfin. Anything playing outranks
// anything paused, so a paused speaker gives way to Jellyfin starting.
constexpr char MEDIA_SPEAKER[]  = "media_player.office_speaker";
constexpr int  JELLYFIN_COVER_H = 300;  // enough for the card's frame, square from the middle
constexpr int  JELLYFIN_PRESET  = 1;  // holding the card for Jellyfin: Preset 2
std::atomic<bool> s_on_jellyfin{false};  // what the card shows, and so controls

// The favourites, in the order the popup shows them. For another: open it in
// the Spotify app, Share, Copy link; open.spotify.com/playlist/ID is
// spotify:playlist:ID here. A Daily Mix keeps its link as its songs change.
constexpr const char *PICK_URIS[] = {
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // Top 50 - Global
    "spotify:playlist:37i9dQZEVXbKCF6dqVpDkS",  // Top 50 - Netherlands
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // Hip Hop Mix
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // Daily Mix 2
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // Daily Mix 3
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // Daily Mix 5
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // Daily Mix 6
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // On Repeat
};
static_assert(std::size(PICK_URIS) <= media::kPickCount, "more favourites than the popup holds");

// Titles and covers change now and then, a Daily Mix's among them.
constexpr int           PICKS_REFRESH_MS  = 6 * units::kSecondsPerHour * units::kMsPerSecond;
constexpr int           PICKS_RETRY_MS    = 30 * units::kMsPerSecond;
constexpr int           EMBED_TIMEOUT_MS  = 8 * units::kMsPerSecond;
constexpr std::size_t   EMBED_MAX         = 8 * units::kBytesPerKiB;

/** Fraction of full scale, per press. */
constexpr float VOLUME_STEP = 0.05f;

constexpr float PERCENT_PER_WHOLE = 100.0f;

// attribute_number()'s answer when there is no number to read.
constexpr float NO_NUMBER = -1.0f;

constexpr std::size_t ISO_SECONDS_LENGTH = sizeof("YYYY-MM-DDTHH:MM:SS") - 1;
constexpr int         MAX_POSITION_AGE_S = units::kSecondsPerDay;

constexpr std::size_t SERVICE_VALUE_SIZE = 16;
constexpr int         TENTHS_PER_DEGREE  = 10;

bool is_on(const std::string &state)
{
    return state == "on" || state == "playing" || state == "heat" || state == "cool";
}

bool known(const hass::ws::Entity *entity)
{
    return entity != nullptr && entity->state != "unavailable" && entity->state != "unknown";
}

std::string display_value(const hass::ws::Entity &entity)
{
    if (!known(&entity)) {
        return "--";
    }
    if (!entity.unit.empty()) {
        return entity.state + " " + entity.unit;
    }
    return entity.state;
}

const char *on_off(const hass::ws::Entity *entity)
{
    if (!known(entity)) {
        return "--";
    }
    return is_on(entity->state) ? "ON" : "OFF";
}

std::string s_position_stamp;
int         s_position_duration = -1;
bool        s_position_playing  = false;

/** Seconds since a Home Assistant timestamp, or 0 if it cannot be read. */
int seconds_since(const std::string &iso)
{
    std::tm parsed{};
    if (iso.size() < ISO_SECONDS_LENGTH ||
        strptime(iso.c_str(), "%Y-%m-%dT%H:%M:%S", &parsed) == nullptr) {
        return 0;
    }
    const std::time_t when = rtc::utc_seconds(parsed);
    const std::time_t now  = std::time(nullptr);
    const double age = std::difftime(now, when);
    return age > 0.0 && age < MAX_POSITION_AGE_S ? static_cast<int>(age) : 0;
}

std::atomic<bool> s_climate_on{false};
std::atomic<bool> s_all_lights_on{false};
std::atomic<bool> s_light_on[LIGHT_COUNT];
std::atomic<bool> s_toggle_on[TOGGLE_COUNT];
std::atomic<bool> s_muted{false};
std::atomic<int>  s_volume_pct{-1};  // negative until the speaker reports one

// What was set stands until the player reports it, or this long: a player
// reports slowly and late, and a drag's earlier levels came back after it.
constexpr std::int64_t VOLUME_HOLD_US    = 10 * units::kUsPerSecond;
constexpr int          VOLUME_AGREES     = 1;  // percent either way, for rounding
constexpr std::int64_t MEDIA_GONE_US     = 3 * units::kUsPerSecond;
constexpr std::int64_t VOLUME_MIN_GAP_US = 250 * units::kUsPerMs;

std::atomic<int>          s_volume_pending{-1};
std::atomic<std::int64_t> s_volume_sent_us{0};
esp_timer_handle_t        s_volume_timer = nullptr;
std::atomic<std::int64_t> s_volume_set_us{0};  // 0 once the player agreed
std::atomic<int>          s_volume_set{-1};
std::atomic<int>          s_entity_count{0};

int percent_of(float fraction)
{
    return static_cast<int>(fraction * PERCENT_PER_WHOLE + 0.5f);
}

std::string attribute(const hass::ws::Entity &entity, const char *key)
{
    const auto it = entity.attributes.find(key);
    return it == entity.attributes.end() ? std::string{} : it->second;
}

float attribute_number(const hass::ws::Entity &entity, const char *key)
{
    const auto it = entity.attributes.find(key);
    if (it == entity.attributes.end()) {
        return NO_NUMBER;
    }
    char       *end   = nullptr;
    const float value = std::strtof(it->second.c_str(), &end);
    return end == it->second.c_str() ? NO_NUMBER : value;
}

ui::Level pill_level(const PillSpec &spec, const hass::ws::Entity *entity)
{
    if (!known(entity)) {
        return ui::Level::Neutral;
    }
    char       *end   = nullptr;
    const float value = std::strtof(entity->state.c_str(), &end);
    if (end == entity->state.c_str()) {
        return ui::Level::Neutral;
    }
    if (value >= spec.good_lo && value <= spec.good_hi) {
        return ui::Level::Good;
    }
    return value >= spec.warn_lo && value <= spec.warn_hi ? ui::Level::Warn : ui::Level::Bad;
}

std::string upper(std::string text)
{
    for (char &c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

/** "script.foo" is called as domain "script", service "foo". */
void run_script(const char *script)
{
    const std::string full{script};
    const std::size_t dot = full.find('.');
    if (dot == std::string::npos) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service("script", full.substr(dot + 1).c_str(), script));
}

void render_thermostat(const hass::ws::EntityStore &store)
{
    const hass::ws::Entity *climate = store.find(CLIMATE_ENTITY);
    if (climate == nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat(-1.0f, -1.0f, "--", ui::Hvac::Off));
        return;
    }

    const float min_c  = attribute_number(*climate, "min_temp");
    const float max_c  = attribute_number(*climate, "max_temp");
    const float step_c = attribute_number(*climate, "target_temp_step");
    if (min_c > 0.0f && max_c > min_c) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_thermostat_range(min_c, max_c, step_c));
    }

    const auto action  = climate->attributes.find("hvac_action");
    const bool heating = action != climate->attributes.end() ? action->second == "heating"
                                                             : is_on(climate->state);
    const ui::Hvac state = climate->state == "off" ? ui::Hvac::Off
                                                   : (heating ? ui::Hvac::Heating : ui::Hvac::Idle);
    s_climate_on.store(state != ui::Hvac::Off, std::memory_order_relaxed);

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_thermostat(attribute_number(*climate, "current_temperature"),
                           attribute_number(*climate, "temperature"),
                           upper(climate->state).c_str(), state));
}

std::string episode_line(const std::string &series, int season, int episode)
{
    std::string line = series;
    if (!line.empty() && season > 0 && episode > 0) {
        line += "\nSeason " + std::to_string(season) + ", episode " + std::to_string(episode);
    }
    return line;
}

std::string media_artist(const hass::ws::Entity &player)
{
    std::string artist = attribute(player, "media_artist");
    if (!artist.empty()) {
        return artist;
    }
    // An episode has a series where a song has an artist.
    return episode_line(attribute(player, "media_series_title"),
                        static_cast<int>(attribute_number(player, "media_season")),
                        static_cast<int>(attribute_number(player, "media_episode")));
}

// What the card shows of a player, whichever it is: the speaker as Home
// Assistant reports it, Jellyfin as its own socket does.
struct PlayerView {
    bool        known    = false;
    bool        jellyfin = false;
    std::string state;         // playing, paused, idle, off
    std::string source;
    std::string title;
    std::string artist;
    std::string picture;       // a path on Home Assistant, or a whole address
    std::string position_key;  // changes whenever the position is reported afresh
    int         position_s = 0;
    int         duration_s = 0;
    float       volume     = NO_NUMBER;
    bool        muted      = false;
    std::string episode;       // a Jellyfin episode, whose segments are asked for
    std::string series;        // its series, whose episodes are its neighbours
    std::string still;         // a video's own picture, for the cinema view
};

bool view_going(const PlayerView &view)
{
    return view.known &&
           (view.state == "playing" || view.state == "paused" || view.state == "buffering");
}

PlayerView speaker_view(const hass::ws::Entity *speaker)
{
    PlayerView view;
    if (!known(speaker)) {
        return view;
    }
    const std::string app = attribute(*speaker, "app_name");
    view.known            = true;
    view.state            = speaker->state;
    view.source           = upper(app.empty() ? speaker->name : app);
    view.title            = attribute(*speaker, "media_title");
    view.artist           = media_artist(*speaker);
    view.picture          = attribute(*speaker, "entity_picture_local");
    if (view.picture.empty()) {
        view.picture = attribute(*speaker, "entity_picture");
    }
    view.position_key    = attribute(*speaker, "media_position_updated_at");
    view.duration_s      = static_cast<int>(attribute_number(*speaker, "media_duration"));
    const int reported   = static_cast<int>(attribute_number(*speaker, "media_position"));
    view.position_s      = reported + (speaker->state == "playing" ? seconds_since(view.position_key) : 0);
    view.volume          = attribute_number(*speaker, "volume_level");
    view.muted           = attribute(*speaker, "is_volume_muted") == "true";
    return view;
}

PlayerView jellyfin_view(const jellyfin::NowPlaying &now)
{
    PlayerView view;
    view.jellyfin = true;
    if (!now.active) {
        return view;
    }
    view.known        = true;
    view.state        = now.paused ? "paused" : "playing";
    view.source       = "JELLYFIN";
    view.title        = now.title;
    view.artist       = episode_line(now.series, now.season, now.episode);
    // An episode's own picture is a still from it: the card shows its season's
    // poster, or its series', and the cinema view the still.
    const std::string &poster = !now.season_id.empty() ? now.season_id
                              : !now.series_id.empty() ? now.series_id
                                                       : now.item;
    view.picture = jellyfin::cover_url(now.kind == "Episode" ? poster : now.item, JELLYFIN_COVER_H);
    view.still   = jellyfin::cover_url(now.item, JELLYFIN_COVER_H);
    view.position_s   = now.position_s;
    view.duration_s   = now.duration_s;
    view.position_key = now.item + ':' + std::to_string(now.position_s) + (now.paused ? "p" : "");
    view.volume       = now.volume >= 0 ? static_cast<float>(now.volume) / PERCENT_PER_WHOLE : NO_NUMBER;
    view.episode      = now.kind == "Episode" ? now.item : "";
    view.series       = now.series_id;
    return view;
}

// Both sources draw the card from tasks of their own, one at a time.
std::mutex s_media_lock;
PlayerView s_speaker_view;
PlayerView s_jellyfin_view;

/** The player the card shows and controls: anything playing before anything
 *  paused, the speaker before Jellyfin; the speaker while neither has anything. */
const PlayerView &choose_view()
{
    const PlayerView *order[] = {&s_speaker_view, &s_jellyfin_view};
    const PlayerView *chosen  = &s_speaker_view;
    bool              found   = false;
    for (const bool want_playing : {true, false}) {
        for (const PlayerView *view : order) {
            if (!found && view_going(*view) && (view->state == "paused") != want_playing) {
                chosen = view;
                found  = true;
            }
        }
    }
    if (s_on_jellyfin.exchange(chosen->jellyfin) != chosen->jellyfin) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_media_hold_preset(chosen->jellyfin ? JELLYFIN_PRESET : -1));
    }
    return *chosen;
}

/** A player that vanishes or loses its title for a moment is not shown as
 *  gone until it has stayed so for a while. */
bool gone_only_briefly(const PlayerView &view)
{
    const bool bare = view.known && view.state != "off" && view.state != "idle" &&
                      view.title.empty();

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

void show_media_text(const std::string &source, const std::string &title,
                     const std::string &artist, const std::string &state, bool playing,
                     bool art_coming)
{
    static std::string s_shown;
    const std::string  shown = source + '\n' + title + '\n' + artist + '\n' + state +
                              (playing ? "\n1" : "\n0");
    if (shown != s_shown) {
        s_shown = shown;
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media(source.c_str(), title.c_str(), artist.c_str(),
                                                    state.c_str(), playing,
                                                    state != "OFF" && state != "--", art_coming));
    }
}

void show_media_position(const PlayerView &view, bool playing)
{
    if (view.position_key == s_position_stamp && view.duration_s == s_position_duration &&
        playing == s_position_playing) {
        return;
    }
    s_position_stamp    = view.position_key;
    s_position_duration = view.duration_s;
    s_position_playing  = playing;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_progress(view.position_s, view.duration_s, playing));
}

void show_media_volume(const PlayerView &view)
{
    if (view.volume < 0.0f) {
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

/** Asks for the track's cover once it changed; true when a different one is
 *  now on its way, which the title waits for. */
bool ask_for_art(const std::string &picture, const std::string &title)
{
    static std::string s_art_title;
    static std::string s_art_path;
    static bool        s_art_asked = false;
    if (title != s_art_title) {
        s_art_title = title;
        s_art_asked = false;
    }
    if (s_art_asked || (picture.empty() && !title.empty())) {
        return false;
    }
    s_art_asked             = true;
    const std::string path  = title.empty() ? "" : picture;
    const bool        other = !path.empty() && path != s_art_path;
    s_art_path              = path;
    media::set_art_path(path.c_str());
    return other;
}

void want_segments(const std::string &episode, const std::string &series, bool video);

/** With s_media_lock held. */
void show_media()
{
    const PlayerView &view = choose_view();
    want_segments(view.jellyfin ? view.episode : "", view.series, view.jellyfin);
    media::set_still_url(view.jellyfin ? view.still.c_str() : "");
    if (gone_only_briefly(view)) {
        return;
    }
    if (!view.known) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("SPEAKER", "", "", "--", false, false));
        media::set_art_path("");
        return;
    }
    const bool        playing = view.state == "playing";
    // What an idle player still names is what it last played: nothing is on.
    const bool        on      = view_going(view);
    const std::string title   = on ? view.title : "";
    // An idle speaker has nothing on: to look at, it is off.
    const std::string state   = view.state == "idle" ? "OFF" : upper(view.state);
    if (!view.jellyfin) {
        s_muted.store(view.muted, std::memory_order_relaxed);
    }
    const bool art_coming = ask_for_art(view.picture, title);
    show_media_text(view.source, title, on ? view.artist : "", state, playing, art_coming);
    show_media_position(view, playing);
    show_media_volume(view);
}

void render_media(const hass::ws::EntityStore &store)
{
    std::lock_guard<std::mutex> hold(s_media_lock);
    s_speaker_view = speaker_view(store.find(MEDIA_SPEAKER));
    show_media();
}

// The favourites, locked: the lookup task names them and a tap reads them on
// the LVGL task.
std::mutex               s_picks_lock;
std::vector<picks::Pick> s_picks;

void show_picks()
{
    std::lock_guard<std::mutex> hold(s_picks_lock);
    for (int i = 0; i < media::kPickCount; ++i) {
        const bool here = i < static_cast<int>(s_picks.size());
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pick(i, here ? s_picks[i].name.c_str() : ""));
        media::set_pick_art(i, here ? s_picks[i].image.c_str() : "");
    }
}

bool fetch_text(const std::string &url, std::string &out)
{
    net::HostConfig host;
    host.timeout_ms  = EMBED_TIMEOUT_MS;
    host.connections = 1;
    net::Request request;
    request.host     = net::host_for(url, host);
    request.path     = url;
    request.priority = net::Priority::Now;
    request.max_body = EMBED_MAX;
    request.what     = "favourite";
    return net::fetch(std::move(request), out).ok() && !out.empty();
}

// A video's intro and credits, asked of Jellyfin when an episode starts, so the
// card can offer skipping them. On a task of its own: a slow server must not
// hold up the socket the page is drawn from.
std::mutex   s_segments_lock;
std::string  s_segments_item;  // the episode wanted, empty for none
std::string  s_segments_series;
jellyfin::Neighbours s_neighbours;  // the episodes either side, once looked up
jobs::Job s_segments_job = jobs::kNoJob;

void show_segments(const std::vector<segments::Segment> &found)
{
    ui::MediaSegment shown[ui::kMaxSegments];
    int              count = 0;
    for (const segments::Segment &segment : found) {
        if (count < ui::kMaxSegments) {
            shown[count++] = {segment.kind == segments::Kind::Intro ? ui::MediaSegment::Kind::Intro
                                                                    : ui::MediaSegment::Kind::Credits,
                              segment.start_s, segment.end_s};
        }
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_segments(shown, count));
}

jobs::Result look_up_segments()
{
    std::string item;
    std::string series;
    {
        std::lock_guard<std::mutex> hold(s_segments_lock);
        item   = s_segments_item;
        series = s_segments_series;
    }
    jellyfin::Neighbours around;
    std::string          answer;
    if (!item.empty() && !series.empty() &&
        jellyfin::fetch(jellyfin::neighbours_path(series, item), answer)) {
        around = jellyfin::neighbours(answer, item);
    }
    {
        std::lock_guard<std::mutex> hold(s_segments_lock);
        s_neighbours = around;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_media_neighbours(!around.previous.empty(), !around.next.empty()));
    if (item.empty() || !jellyfin::fetch(segments::path_for(item), answer)) {
        show_segments({});
        return jobs::sleep();
    }
    const std::vector<segments::Segment> found = segments::parse(answer);
    ESP_LOGI(TAG, "%u segments for the episode", static_cast<unsigned>(found.size()));
    show_segments(found);
    return jobs::sleep();
}

/** Asks for a new episode's segments, or drops them when no episode shows. */
void want_segments(const std::string &episode, const std::string &series, bool video)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_seeks(video));
    {
        std::lock_guard<std::mutex> hold(s_segments_lock);
        if (episode == s_segments_item) {
            return;
        }
        s_segments_item   = episode;
        s_segments_series = series;
    }
    if (s_segments_job == jobs::kNoJob) {
        jobs::Spec spec;
        spec.name     = "segments";
        spec.lane     = jobs::Lane::Slow;
        spec.first_ms = jobs::kNever;
        spec.run      = look_up_segments;
        s_segments_job = jobs::add(std::move(spec));
    }
    jobs::poke(s_segments_job);
}

/** Looks each favourite up now and then, and shows what it found. */
jobs::Result look_up_picks()
{
    bool all = true;
    for (std::size_t i = 0; i < std::size(PICK_URIS); ++i) {
        picks::Pick pick{PICK_URIS[i], "", ""};
        std::string answer;
        if (fetch_text(picks::embed_url(pick.uri), answer) && picks::take_embed(answer, pick)) {
            std::lock_guard<std::mutex> hold(s_picks_lock);
            s_picks[i] = pick;
        } else {
            all = false;
            ESP_LOGW(TAG, "no title for %s yet", PICK_URIS[i]);
        }
    }
    show_picks();
    return all ? jobs::done() : jobs::failed();
}

/** From the first render, by when the covers can be fetched. */
void start_picks()
{
    static bool started = false;
    if (std::exchange(started, true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> hold(s_picks_lock);
        s_picks.clear();
        for (const char *uri : PICK_URIS) {
            s_picks.push_back({uri, "", ""});
        }
    }
    jobs::Spec spec;
    spec.name         = "picks";
    spec.lane         = jobs::Lane::Slow;
    spec.period_ms    = PICKS_REFRESH_MS;
    spec.online       = true;
    spec.retry_ms     = PICKS_RETRY_MS;
    spec.max_retry_ms = PICKS_RETRY_MS;
    spec.run          = look_up_picks;
    jobs::add(std::move(spec));
}

void on_played(const cJSON *result)
{
    if (result == nullptr) {
        ESP_LOGW(TAG, "the favourite did not start");
    }
}

void send_volume(void *)
{
    const int percent = s_volume_pending.exchange(-1, std::memory_order_relaxed);
    if (percent < 0) {
        return;
    }
    s_volume_sent_us.store(esp_timer_get_time(), std::memory_order_relaxed);
    if (s_on_jellyfin.load(std::memory_order_relaxed)) {
        jellyfin::set_volume(percent);
        return;
    }
    char value[SERVICE_VALUE_SIZE];
    std::snprintf(value, sizeof(value), "%.2f",
                  static_cast<double>(percent) / static_cast<double>(PERCENT_PER_WHOLE));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service_with("media_player", "volume_set",
                                                              MEDIA_SPEAKER, "volume_level", value));
}

}  // namespace

void on_media_volume(int percent)
{
    percent = std::clamp(percent, 0, static_cast<int>(PERCENT_PER_WHOLE));
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
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_timer_start_once(s_volume_timer, VOLUME_MIN_GAP_US - since));
    }
}

void nudge_volume(float delta)
{
    const int reported = s_volume_pct.load(std::memory_order_relaxed);
    if (reported < 0) {
        return;  // a blind guess would jump the volume
    }
    float wanted = static_cast<float>(reported) / PERCENT_PER_WHOLE + delta;
    wanted       = wanted < 0.0f ? 0.0f : (wanted > 1.0f ? 1.0f : wanted);
    on_media_volume(percent_of(wanted));
}

void play_neighbour(bool next)
{
    std::string item;
    {
        std::lock_guard<std::mutex> hold(s_segments_lock);
        item = next ? s_neighbours.next : s_neighbours.previous;
    }
    jellyfin::play_now(item);
}

void on_jellyfin(const jellyfin::NowPlaying &now)
{
    std::lock_guard<std::mutex> hold(s_media_lock);
    s_jellyfin_view = jellyfin_view(now);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_media_subtitles(now.active && now.subtitle_track >= 0, now.active && now.subtitle >= 0));
    show_media();
}

void on_seek(int position_s)
{
    if (s_on_jellyfin.load(std::memory_order_relaxed)) {
        jellyfin::seek(position_s);
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service_with(
        "media_player", "media_seek", MEDIA_SPEAKER, "seek_position",
        std::to_string(position_s).c_str()));
}

void on_pick(int index)
{
    picks::Pick pick;
    {
        std::lock_guard<std::mutex> hold(s_picks_lock);
        if (index < 0 || index >= static_cast<int>(s_picks.size())) {
            return;
        }
        pick = s_picks[index];
    }
    ESP_LOGI(TAG, "playing %s", pick.name.c_str());
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::request(picks::play_request(pick, MEDIA_SPEAKER), on_played));
}

void on_media(ui::MediaAction action)
{
    switch (action) {
        case ui::MediaAction::PlayPause:
            if (s_on_jellyfin.load(std::memory_order_relaxed)) {
                jellyfin::play_pause();
            } else {
                hass::ws::call_service("media_player", "media_play_pause", MEDIA_SPEAKER);
            }
            break;
        case ui::MediaAction::Previous:
        case ui::MediaAction::Next:
            if (s_on_jellyfin.load(std::memory_order_relaxed)) {
                play_neighbour(action == ui::MediaAction::Next);
            } else {
                hass::ws::call_service("media_player",
                                       action == ui::MediaAction::Next ? "media_next_track"
                                                                       : "media_previous_track",
                                       MEDIA_SPEAKER);
            }
            break;
        case ui::MediaAction::VolumeDown:
            nudge_volume(-VOLUME_STEP);
            break;
        case ui::MediaAction::VolumeUp:
            nudge_volume(VOLUME_STEP);
            break;
        case ui::MediaAction::Mute: {
            const bool muted = s_muted.load(std::memory_order_relaxed);
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                hass::ws::call_service_with("media_player", "volume_mute", MEDIA_SPEAKER,
                                            "is_volume_muted", muted ? "false" : "true"));
            break;
        }
        case ui::MediaAction::Subtitles:
            if (s_on_jellyfin.load(std::memory_order_relaxed)) {
                jellyfin::toggle_subtitles();
            }
            break;
    }
}

void init()
{
    const esp_timer_create_args_t volume_timer = {
        .callback              = send_volume,
        .arg                   = nullptr,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "volume",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&volume_timer, &s_volume_timer));
    show_unknown();
}

void render(const hass::ws::EntityStore &store)
{
    s_entity_count.store(static_cast<int>(store.size()), std::memory_order_relaxed);

    if (const hass::ws::Entity *home = store.find("zone.home"); home != nullptr) {
        const float lat = attribute_number(*home, "latitude");
        const float lon = attribute_number(*home, "longitude");
        if (lat != NO_NUMBER && lon != NO_NUMBER) {
            radar::set_home(lat, lon);
        }
    }

    render_thermostat(store);
    start_picks();

    for (int i = 0; i < PILL_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(PILLS[i].entity);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_pill(i, PILLS[i].label,
                         entity != nullptr ? display_value(*entity).c_str() : "--",
                         pill_level(PILLS[i], entity)));
    }
    for (int i = PILL_COUNT; i < ui::kPillCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pill(i, "", "", ui::Level::Neutral));
    }

    for (int i = 0; i < TOGGLE_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(TOGGLES[i].entity);
        const bool              on     = entity != nullptr && is_on(entity->state);
        s_toggle_on[i].store(on, std::memory_order_relaxed);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_dial_toggle(i, toggle_label(TOGGLES[i], on), on));
    }
    for (int i = TOGGLE_COUNT; i < ui::kDialToggleCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_dial_toggle(i, "", false));
    }

    const hass::ws::Entity *all = store.find(ALL_LIGHTS_ENTITY);
    s_all_lights_on.store(all != nullptr && is_on(all->state), std::memory_order_relaxed);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_lights("LIGHTS", on_off(all), all != nullptr && is_on(all->state)));

    for (int i = 0; i < LIGHT_COUNT; ++i) {
        const hass::ws::Entity *entity = store.find(LIGHTS[i].entity);
        const bool              on     = entity != nullptr && is_on(entity->state);
        s_light_on[i].store(on, std::memory_order_relaxed);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, LIGHTS[i].name, on_off(entity), on));
    }
    for (int i = LIGHT_COUNT; i < ui::kLightCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(i, "", "", false));
    }

    render_media(store);
}

void on_setpoint(float celsius)
{
    ESP_LOGI(TAG, "setpoint %d.%d C", static_cast<int>(celsius),
             static_cast<int>(celsius * TENTHS_PER_DEGREE) % TENTHS_PER_DEGREE);
    char value[SERVICE_VALUE_SIZE];
    std::snprintf(value, sizeof(value), "%.1f", celsius);
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service_with("climate", "set_temperature",
                                                              CLIMATE_ENTITY, "temperature",
                                                              value));
}

void on_mode()
{
    const bool  currently_on = s_climate_on.load(std::memory_order_relaxed);
    const char *mode         = currently_on ? "off" : "heat";
    ESP_LOGI(TAG, "hvac mode -> %s", mode);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service_with("climate", "set_hvac_mode", CLIMATE_ENTITY, "hvac_mode", mode));
}

void on_lights()
{
    const bool currently_on = s_all_lights_on.load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "all lights -> %s", currently_on ? "off" : "on");

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_lights("LIGHTS", currently_on ? "OFF" : "ON", !currently_on));
    run_script(currently_on ? ALL_LIGHTS_OFF : ALL_LIGHTS_ON);
}

void on_light(int index)
{
    if (index < 0 || index >= LIGHT_COUNT) {
        return;
    }
    const bool currently_on = s_light_on[index].load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "light %s -> %s", LIGHTS[index].name, currently_on ? "off" : "on");

    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_light(index, LIGHTS[index].name,
                                                currently_on ? "OFF" : "ON", !currently_on));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service("light", "toggle", LIGHTS[index].entity));
}

void on_dial_toggle(int index)
{
    if (index < 0 || index >= TOGGLE_COUNT) {
        return;
    }
    const bool currently_on = s_toggle_on[index].load(std::memory_order_relaxed);
    ESP_LOGI(TAG, "toggle %s -> %s", TOGGLES[index].entity, currently_on ? "off" : "on");

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::set_dial_toggle(index, toggle_label(TOGGLES[index], !currently_on), !currently_on));
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        hass::ws::call_service("input_boolean", "toggle", TOGGLES[index].entity));
}

std::vector<std::string> entities()
{
    std::vector<std::string> out = {CLIMATE_ENTITY, ALL_LIGHTS_ENTITY, ALL_LIGHTS_ON,
                                    ALL_LIGHTS_OFF, MEDIA_SPEAKER,
                                    "zone.home"};  // the radar's centre
    for (const PillSpec &pill : PILLS) {
        out.emplace_back(pill.entity);
    }
    for (const LightSpec &light : LIGHTS) {
        out.emplace_back(light.entity);
    }
    for (const ToggleSpec &toggle : TOGGLES) {
        out.emplace_back(toggle.entity);
    }
    return out;
}

std::vector<std::string> attributes()
{
    return {"min_temp",          "max_temp",           "target_temp_step",
            "hvac_action",       "current_temperature", "temperature",
            "media_title",       "media_artist",        "media_series_title",
            "media_season",      "media_episode",       "app_name",
            "is_volume_muted",   "media_position_updated_at",
            "media_duration",    "media_position",      "volume_level",
            "entity_picture_local", "entity_picture",   "latitude",
            "longitude"};
}

int entity_count()
{
    return s_entity_count.load(std::memory_order_relaxed);
}

}  // namespace room
