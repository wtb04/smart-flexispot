#include "media_stub.h"

#include "esp_timer.h"
#include "media.h"  // components/media: the sizes the screen takes

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace media_stub {
namespace {
struct Track {
    const char *title;
    const char *artist;
    int         length_s;
    float       hue;  // of its made-up cover
};
constexpr Track TRACKS[] = {
    {"Midnight City", "M83", 243, 0.62f},
    {"Something About Us", "Daft Punk", 232, 0.08f},
    {"Teardrop", "Massive Attack", 330, 0.83f},
};

struct Episode {
    const char *title;
    int         number;
    int         length_s;
    float       hue;
};
constexpr const char *SERIES  = "Parks and Recreation";
constexpr int         SEASON  = 3;
constexpr Episode     EPISODES[] = {
    {"Road Trip", 12, 1310, 0.10f},
    {"The Fight", 13, 1504, 0.55f},
    {"Time Capsule", 14, 1290, 0.33f},
};
constexpr int JELLYFIN_PRESET = 1;  // holding the card for Jellyfin, as room.cpp has it

// Intro and credits, as Jellyfin's media segments give them.
constexpr int INTRO_FROM_S   = 20;
constexpr int INTRO_TO_S     = 95;
constexpr int CREDITS_BEFORE = 64;  // seconds from the end

// Jellyfin and Home Assistant report the position now and then, not continuously.
constexpr std::int64_t REPORT_EVERY_MS = 10000;

constexpr const char *PICKS[] = {"Top 50 - Global", "Top 50 - Netherlands", "Hip Hop Mix", "Daily Mix 2",
                                 "Daily Mix 3",     "Daily Mix 5",          "Daily Mix 6", "On Repeat"};

Scene        s_scene   = Scene::Idle;
int          s_item    = 0;  // the track, or the episode
bool         s_playing = false;
int          s_volume  = 35;
bool         s_muted   = false;
bool         s_subtitles = false;
double       s_position_s = 0;   // at s_position_at
std::int64_t s_position_at = 0;  // ms
std::int64_t s_reported_at = 0;

std::vector<std::uint16_t> s_art;    // shown cover
std::vector<std::uint16_t> s_still;  // cinema's still
std::vector<std::uint16_t> s_pick_art[media::kPickCount];

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

std::uint16_t rgb565(float r, float g, float b)
{
    const auto part = [](float v, int bits) {
        return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 1.0f) * ((1 << bits) - 1) + 0.5f);
    };
    return static_cast<std::uint16_t>(part(r, 5) << 11 | part(g, 6) << 5 | part(b, 5));
}

// A hue to a colour, dimmed by `light`.
void colour_of(float hue, float light, float &r, float &g, float &b)
{
    const float h = (hue - std::floor(hue)) * 6.0f;
    const float x = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
    const float table[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
    const float *c = table[static_cast<int>(h) % 6];
    r = c[0] * light;
    g = c[1] * light;
    b = c[2] * light;
}

// A cover to look at, not a picture of anything: a gradient with a disc on it.
void paint(std::vector<std::uint16_t> &into, int w, int h, float hue)
{
    into.resize(static_cast<std::size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) / w, fy = static_cast<float>(y) / h;
            const float dx = fx - 0.62f, dy = fy - 0.4f;
            const bool  disc = dx * dx + dy * dy < 0.05f;
            float       r, g, b;
            colour_of(hue + (disc ? 0.08f : fy * 0.12f), disc ? 0.95f : 0.35f + 0.45f * (1.0f - fy), r, g, b);
            into[static_cast<std::size_t>(y) * w + x] = rgb565(r, g, b);
        }
    }
}

int length_s()
{
    return s_scene == Scene::Music ? TRACKS[s_item].length_s : s_scene == Scene::Episode ? EPISODES[s_item].length_s : 0;
}

int position_s()
{
    const double at = s_position_s + (s_playing ? (now_ms() - s_position_at) / 1000.0 : 0.0);
    return static_cast<int>(std::min<double>(at, length_s()));
}

void set_position(int position_s)
{
    s_position_s  = position_s;
    s_position_at = now_ms();
}

void report_progress()
{
    s_reported_at = now_ms();
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_progress(position_s(), length_s(), s_playing));
}

void show_text()
{
    const char *state = s_playing ? "PLAYING" : "PAUSED";
    if (s_scene == Scene::Music) {
        const Track &t = TRACKS[s_item];
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("SPOTIFY", t.title, t.artist, state, s_playing, true));
    } else if (s_scene == Scene::Episode) {
        const Episode &e = EPISODES[s_item];
        char           line[96];
        std::snprintf(line, sizeof(line), "%s\nSeason %d, episode %d", SERIES, SEASON, e.number);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("JELLYFIN", e.title, line, state, s_playing, true));
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media("OFFICE SPEAKER", "", "", "OFF", false, false));
    }
}

void show_item()
{
    const bool video = s_scene == Scene::Episode;
    show_text();
    if (s_scene == Scene::Idle) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_album_art(nullptr, false));
    } else {
        paint(s_art, media::kArtSize, media::kArtSize, video ? EPISODES[s_item].hue : TRACKS[s_item].hue);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_album_art(s_art.data(), false));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_hold_preset(video ? JELLYFIN_PRESET : -1));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_seeks(video));
    if (video) {
        const int              end = EPISODES[s_item].length_s;
        const ui::MediaSegment segments[] = {
            {ui::MediaSegment::Kind::Intro, INTRO_FROM_S, INTRO_TO_S},
            {ui::MediaSegment::Kind::Credits, end - CREDITS_BEFORE, end},
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_segments(segments, 2));
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_media_neighbours(s_item > 0, s_item + 1 < static_cast<int>(std::size(EPISODES))));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_subtitles(true, s_subtitles));
        paint(s_still, media::kStillW, media::kStillH, EPISODES[s_item].hue + 0.3f);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_cinema_still(s_still.data()));
    } else {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_segments(nullptr, 0));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_neighbours(false, false));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_subtitles(false, false));
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_cinema_still(nullptr));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(s_muted ? 0 : s_volume));
    report_progress();
}

void go_to(int item, int from_s)
{
    s_item = item;
    set_position(from_s);
    show_item();
}
}  // namespace

void start()
{
    for (int i = 0; i < media::kPickCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pick(i, PICKS[i]));
        paint(s_pick_art[i], media::kPickArtSize, media::kPickArtSize, static_cast<float>(i) / media::kPickCount);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_pick_art(i, s_pick_art[i].data()));
    }
    show(Scene::Idle);
}

void tick()
{
    if (s_scene == Scene::Idle || !s_playing) {
        return;
    }
    if (position_s() >= length_s()) {
        const int last = s_scene == Scene::Music ? static_cast<int>(std::size(TRACKS)) : static_cast<int>(std::size(EPISODES));
        if (s_item + 1 < last) {
            go_to(s_item + 1, 0);  // as a queue or Jellyfin's next up goes on
        } else {
            s_playing = false;
            show_item();
        }
        return;
    }
    if (now_ms() - s_reported_at >= REPORT_EVERY_MS) {
        report_progress();
    }
}

void show(Scene scene)
{
    s_scene   = scene;
    s_playing = scene != Scene::Idle;
    // An episode starts just before its intro, so Skip intro can be tried.
    go_to(scene == Scene::Episode ? 1 : 0, scene == Scene::Episode ? INTRO_FROM_S - 5 : 60);
}

void next_scene()
{
    show(s_scene == Scene::Idle ? Scene::Music : s_scene == Scene::Music ? Scene::Episode : Scene::Idle);
}

void on_media(ui::MediaAction action)
{
    if (s_scene == Scene::Idle) {
        return;
    }
    const int count = s_scene == Scene::Music ? static_cast<int>(std::size(TRACKS)) : static_cast<int>(std::size(EPISODES));
    switch (action) {
        case ui::MediaAction::PlayPause:
            set_position(position_s());
            s_playing = !s_playing;
            show_text();
            report_progress();
            break;
        case ui::MediaAction::Next:     go_to((s_item + 1) % count, 0); break;
        case ui::MediaAction::Previous: go_to((s_item + count - 1) % count, 0); break;
        case ui::MediaAction::VolumeUp:   on_volume(std::min(100, s_volume + 5)); break;
        case ui::MediaAction::VolumeDown: on_volume(std::max(0, s_volume - 5)); break;
        case ui::MediaAction::Mute:
            s_muted = !s_muted;
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(s_muted ? 0 : s_volume));
            break;
        case ui::MediaAction::Subtitles:
            s_subtitles = !s_subtitles;
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_subtitles(true, s_subtitles));
            break;
    }
}

void on_seek(int position)
{
    set_position(std::clamp(position, 0, length_s()));
    report_progress();
}

void on_volume(int percent)
{
    s_volume = percent;
    s_muted  = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_media_volume(percent));
}

// Spotcast starts the favourite on the speaker, which then plays its first track.
void on_pick(int index)
{
    std::printf("I (sim) playing %s\n", PICKS[index]);
    s_scene   = Scene::Music;
    s_playing = true;
    go_to(index % static_cast<int>(std::size(TRACKS)), 0);
}
}  // namespace media_stub
