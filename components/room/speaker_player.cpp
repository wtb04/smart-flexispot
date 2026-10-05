#include "players.h"

#include "esp_log.h"
#include "ha_ws.h"
#include "jobs.h"
#include "media.h"
#include "net.h"
#include "picks.h"
#include "room_layout.h"
#include "units.h"

#include <cstdio>
#include <mutex>
#include <utility>

// The speaker, as Home Assistant has it: whichever app plays on it, Spotify's
// among them, and the favourites the card offers to play there.
namespace room {
namespace {
constexpr char TAG[] = "speaker";

// A media player's supported_features, as Home Assistant numbers them: a Cast
// speaker's change with the app playing on it.
constexpr std::uint32_t TAKES_PAUSE       = 1u << 0;
constexpr std::uint32_t TAKES_VOLUME_SET  = 1u << 2;
constexpr std::uint32_t TAKES_PREVIOUS    = 1u << 4;
constexpr std::uint32_t TAKES_NEXT        = 1u << 5;
constexpr std::uint32_t TAKES_VOLUME_STEP = 1u << 10;
constexpr std::uint32_t TAKES_PLAY        = 1u << 14;

constexpr std::size_t SERVICE_VALUE_SIZE = 16;

static_assert(std::size(PICK_URIS) <= media::kPickCount, "more favourites than the popup holds");

// Titles and covers change now and then, a Daily Mix's among them.
constexpr int         PICKS_REFRESH_MS = 6 * units::kSecondsPerHour * units::kMsPerSecond;
constexpr int         PICKS_RETRY_MS   = 30 * units::kMsPerSecond;
constexpr int         EMBED_TIMEOUT_MS = 8 * units::kMsPerSecond;
constexpr std::size_t EMBED_MAX        = 8 * units::kBytesPerKiB;

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

class Speaker : public Player {
public:
    void take(const hass::ws::Entity *speaker)
    {
        PlayerView view;
        view.pause_settles = true;
        if (!known(speaker)) {
            view_ = view;
            return;
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
        view.position_key  = attribute(*speaker, "media_position_updated_at");
        view.duration_s    = static_cast<int>(attribute_number(*speaker, "media_duration"));
        const int reported = static_cast<int>(attribute_number(*speaker, "media_position"));
        view.position_s    = reported + (speaker->state == "playing" ? seconds_since(view.position_key) : 0);
        view.volume        = attribute_number(*speaker, "volume_level");
        view.muted         = attribute(*speaker, "is_volume_muted") == "true";
        // Without them reported, taken as taking everything, as before they were read.
        const float features = attribute_number(*speaker, "supported_features");
        if (features >= 0.0f) {
            const auto takes = static_cast<std::uint32_t>(features);
            view.remote      = (takes & (TAKES_PAUSE | TAKES_PLAY)) != 0;
            view.tracks_back = (takes & TAKES_PREVIOUS) != 0;
            view.tracks_on   = (takes & TAKES_NEXT) != 0;
            if ((takes & (TAKES_VOLUME_SET | TAKES_VOLUME_STEP)) == 0) {
                view.volume       = kNoNumber;
                view.takes_volume = false;
            }
        }
        view_ = view;
    }

    PlayerView view() const override { return view_; }

    void play_pause() override { hass::ws::call_service("media_player", "media_play_pause", MEDIA_SPEAKER); }

    void seek(int position_s) override
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service_with("media_player", "media_seek", MEDIA_SPEAKER,
                                                                  "seek_position",
                                                                  std::to_string(position_s).c_str()));
    }

    void skip(bool next) override
    {
        hass::ws::call_service("media_player", next ? "media_next_track" : "media_previous_track", MEDIA_SPEAKER);
    }

    void set_volume(int percent) override
    {
        char value[SERVICE_VALUE_SIZE];
        std::snprintf(value, sizeof(value), "%.2f", static_cast<double>(percent) / kPercentPerWhole);
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            hass::ws::call_service_with("media_player", "volume_set", MEDIA_SPEAKER, "volume_level", value));
    }

    void set_muted(bool muted) override
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::call_service_with("media_player", "volume_mute", MEDIA_SPEAKER,
                                                                  "is_volume_muted", muted ? "true" : "false"));
    }

private:
    PlayerView view_;
};

Speaker s_speaker;

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

void on_played(const cJSON *result)
{
    if (result == nullptr) {
        ESP_LOGW(TAG, "the favourite did not start");
    }
}
}  // namespace

Player &speaker_player()
{
    return s_speaker;
}

void take_speaker(const hass::ws::Entity *speaker)
{
    s_speaker.take(speaker);
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

void play_pick(int index)
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(hass::ws::request(picks::play_request(pick, MEDIA_SPEAKER), on_played));
}
}  // namespace room
