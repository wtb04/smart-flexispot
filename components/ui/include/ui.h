#pragma once

#include "deskproto.h"
#include "esp_err.h"
#include "radar.h"
#include "units.h"

#include <cstddef>
#include <cstdint>

namespace ui {
enum class Move : std::int8_t {
    Stop = 0,
    Up   = 1,
    Down = -1,
};

/** Up/Down on press, Stop on release. Must not block: it runs with the LVGL lock held. */
using MoveHandler = void (*)(Move direction);

/** A tap sends the desk to that preset; a long press stores the current height there. */
using PresetHandler = void (*)(int index, bool store);

using BrightnessHandler = void (*)(int percent);

using SetpointHandler = void (*)(float celsius);

using ModeHandler = void (*)();

inline constexpr int kPresetCount = deskproto::kPresetCount;
inline constexpr int kBoxPresets  = deskproto::kBoxPresets;

/** What the screen calls a preset, or "Between" for no preset. Everything off
 *  the screen calls them Preset 1 to 6. */
const char *preset_name(int index);

enum class MediaAction : std::uint8_t {
    PlayPause,
    Previous,
    Next,
    VolumeDown,
    VolumeUp,
    Mute,
    Subtitles,  // on or off
};

using MediaHandler = void (*)(MediaAction action);

inline constexpr int kPillCount = 4;

inline constexpr int kLightCount = 4;

inline constexpr int kDialToggleCount = 2;

enum class Hvac : std::uint8_t {
    Off,
    Idle,     // on, but the setpoint is already met
    Heating,
};

enum class Level : std::uint8_t {
    Neutral,  // no threshold, or nothing reported
    Good,
    Warn,
    Bad,
};

using LightsHandler = void (*)();

using LightHandler = void (*)(int index);

using DialToggleHandler = void (*)(int index);

/** Called when the Setup page comes up, so its readouts can be filled at once. */
using DiagnosticsHandler = void (*)();

enum class Setting : std::uint8_t {
    Charging,
    PresenceGate,
    DeskBluetooth,
    Screen,
    Count,
};

/** The panel has already applied and redrawn it; this is for storing it. */
using SettingHandler = void (*)(Setting setting, bool on);

/** preview is set when the slider is let go, which is the moment to play
 *  something at the chosen level. */
using VolumeHandler = void (*)(int percent, bool preview);

using RestartHandler = void (*)();

/** The accent the panel draws everything in. */
using PrimaryHandler = void (*)(std::uint32_t colour);

/** The screen going dark after a while, and coming back on a touch. The panel
 *  only asks; what that means for the backlight is the board's business. */
using ScreenHandler = void (*)(bool on);

/** True while the rail is against the right edge. */
using RailSideHandler = void (*)(bool right);

/** True while the panel is hung the other way up. */
enum class Orientation : std::uint8_t { Normal, Flipped, Auto };

using OrientationHandler = void (*)(Orientation orientation);

/** showing is true while the scope is the page on screen, reachable is false
 *  once presence gating has hidden it away entirely. */
using RadarHandler = void (*)(bool showing, bool reachable);

/** A plane picked on the radar: find out who it is. */
using DetailsHandler = void (*)(const char *hex, const char *callsign);

/** The focus timer, as the page shows it. Times are milliseconds since boot,
 *  esp_timer's clock: the page counts down against it on its own. */
enum class FocusPhase : std::uint8_t { Idle, Work, Break, LongBreak };
struct Focus {
    FocusPhase   phase;
    int          round;
    int          rounds;
    bool         running;
    std::int64_t ends_at_ms;
    std::int32_t left_ms;
    std::int32_t length_ms;
    int          work_min;
    int          break_min;
    int          long_break_min;
};
enum class FocusAction : std::uint8_t { Toggle, Skip, Reset };
using FocusHandler = void (*)(FocusAction action);
/** A firmware update on its way in or waiting: which board it is for while it
 *  arrives, and whether one is ready for each. */
enum class UpdateTarget : std::uint8_t { None, Panel, Companion };
enum class UpdatePhase : std::uint8_t { Receiving, Installing };
struct UpdateState {
    UpdateTarget busy;
    UpdatePhase  phase;         // arriving here, or going on to the companion
    int          percent;
    int          seconds_left;  // negative until known
    bool         immediate;     // installed once in, rather than kept ready
    bool         panel_ready;
    bool         companion_ready;
};
/** Install what is ready now. */
using UpdateHandler = void (*)();
/** One of the favourites, picked from the media card's popup. */
using PickHandler = void (*)(int index);
/** Jumps the playing video to a position, in seconds. */
using SeekHandler = void (*)(int position_s);

/** The media's volume set outright, as a slider does, in percent. */
using MediaVolumeHandler = void (*)(int percent);

/** Minutes of focus, break and long break, and rounds before the long one. */
using FocusPlanHandler = void (*)(int work_min, int break_min, int long_break_min, int rounds);

/** How long before an appointment its journey is asked for and shown. */
inline constexpr std::int64_t kJourneyAhead = 5 * units::kSecondsPerHour;

/** A card on the diagnostics view. What it covers is the caller's business:
 *  the screen draws a title, a glyph, a summary and named rows. */
enum class Glyph : std::uint8_t { Wifi, Home, Presence, Desk, Link, Power, Radar, Calendar, System };

struct Card {
    const char        *title;
    Glyph              glyph;
    const char *const *rows;  // their names, in order
    int                row_count;
};

inline constexpr int kMaxCards = 9;  // three rows of three
inline constexpr int kMaxRows  = 12;

inline constexpr std::size_t kLogTextMax = 192;

/** One line as the log view shows it. `level` is the logging system's severity
 *  letter: E, W, I or D. */
struct LogLine {
    char level;
    char text[kLogTextMax];
    int  card;  // whose line it is
};

/** Fills `out` with the most recent lines of one card, or of every card when
 *  `card` is negative, oldest first, and returns how many. With `warnings`,
 *  only warnings and errors. Runs on the LVGL task, so it must not block. */
using LogHandler = int (*)(int card, bool warnings, LogLine *out, int max);

/** An empty title means nothing is playing; the card then shows the state.
 *  `controllable` is whether holding the card has anything to act on. Strings
 *  are copied. */
/** With art_coming the text waits for the cover asked for with it, so the two
 *  change together, though never for long. */
esp_err_t set_media(const char *source, const char *title, const char *artist, const char *state,
                    bool playing, bool controllable, bool art_coming = false);

/** What holding the media card does: -1 opens the media panel, as for the
 *  speaker; a preset index sends the desk there instead, as for Jellyfin. */
esp_err_t set_media_hold_preset(int preset);

/** RGB565, media::kArtSize high and `width` wide, square without one, narrower
 *  for a poster. Null hides the art; the buffer must live until it is replaced. */
esp_err_t set_album_art(const void *pixels, bool placeholder, int width = -1);

/** The same cover at media::kLargeArtSize, for the music view, or null for
 *  none; the buffer must live until it is replaced. */
esp_err_t set_album_art_large(const void *pixels);

/** The position is carried forward while playing; a duration of zero hides the bar. */
esp_err_t set_media_progress(int position_s, int duration_s, bool playing);

esp_err_t set_media_volume(int percent);

/** Where a video's intro and credits are, in seconds into it, for the media card
 *  to offer skipping the one and going on after the other. */
struct MediaSegment {
    enum class Kind : std::uint8_t { Intro, Credits };
    Kind kind;
    int  start_s;
    int  end_s;
};
inline constexpr int kMaxSegments = 4;
esp_err_t set_media_segments(const MediaSegment *segments, int count);

/** Whether a swipe across the media card jumps ten seconds, as for a video,
 *  rather than to the next or last track. */
esp_err_t set_media_seeks(bool seeks);

/** Whether a video has an episode before it, and after, for the cinema view to
 *  offer; its Previous and Next actions start them. */
esp_err_t set_media_neighbours(bool previous, bool next);

/** Whether the player takes pause, seek and skip from here. Some, such as
 *  Streamyfin, only report: the card and the cinema view then fade what the
 *  player would not act on. */
esp_err_t set_media_remote(bool remote);

/** Whether a video has subtitles to show, and whether they show. */
esp_err_t set_media_subtitles(bool available, bool shown);

/** A video's own still for the cinema view, media::kStillW by kStillH of
 *  RGB565, or null for none. The buffer must live until it is replaced. */
esp_err_t set_cinema_still(const void *pixels);

/** Whatever is on screen, whole, as RGB565 in PSRAM, for the caller to free
 *  with heap_caps_free; null when it could not be had. Takes the screen's lock. */
std::uint16_t *capture(int &width, int &height);

/** A favourite offered by holding the media card while nothing plays, index
 *  below media::kPickCount. An empty name leaves it out. */
esp_err_t set_pick(int index, const char *name);

/** Its cover, media::kPickArtSize square of RGB565, or null while there is
 *  none. The buffer must live until it is replaced. */
esp_err_t set_pick_art(int index, const void *pixels);

/** An empty label hides the chip, and the remaining ones close up the gap. */
esp_err_t set_pill(int index, const char *label, const char *value, Level level);

/** The bulbs drawn on the button come from set_light(), so both have to be pushed. */
esp_err_t set_lights(const char *label, const char *state, bool on);

/** An empty name hides that button, so fewer lights than slots is fine. */
esp_err_t set_light(int index, const char *name, const char *state, bool on);

/** An empty label hides the toggle. */
esp_err_t set_dial_toggle(int index, const char *label, bool on);

esp_err_t set_thermostat_range(float min_c, float max_c, float step_c);

/** Ignored while a finger is on the dial. A negative target shows as unavailable. */
esp_err_t set_thermostat(float current_c, float target_c, const char *mode, Hvac state);

struct Handlers {
    MoveHandler       move;
    PresetHandler     preset;
    BrightnessHandler brightness;
    MediaHandler      media;
    SetpointHandler   setpoint;
    ModeHandler       mode;
    LightsHandler     lights;
    LightHandler      light;
    DialToggleHandler  dial_toggle;
    DiagnosticsHandler diagnostics;
    SettingHandler     setting;
    VolumeHandler      volume;
    RestartHandler     restart;
    RadarHandler       radar;
    LogHandler         log;
    PrimaryHandler     primary;
    RailSideHandler    rail_side;
    OrientationHandler orientation;
    ScreenHandler      screen;
    DetailsHandler     details;
    FocusHandler       focus;
    FocusPlanHandler   focus_plan;
    UpdateHandler      update_now;
    PickHandler        pick;
    SeekHandler        seek;
    MediaVolumeHandler media_volume;
};

/** Requires the LVGL port to be running. A zero accent keeps the built-in
 *  colour; rail_right puts the rail against the right edge instead of the left.
 *  orientation only tells the page which choice to show; board::init() has
 *  already turned the display, and turns it again as the choice asks. */
esp_err_t init(const Handlers &handlers, int initial_brightness, std::uint32_t accent,
               bool rail_right, Orientation orientation);

/** After init() has put up the splash and the screen is lit: everything else,
 *  built behind the splash a part at a time so that it keeps moving. Nothing
 *  set before this returns is drawn until it has. */
esp_err_t build();

/** A part of the boot has finished: "desk" or "network". */
esp_err_t splash_step(const char *label);

esp_err_t splash_done();

/** Height in millimetres, or negative for "unknown". */
esp_err_t set_height(int height_mm);

esp_err_t set_preset_active(int index, bool active);

/** Unavailable dims and disables every desk control, so nothing can be pressed in vain. */
esp_err_t set_desk_available(bool available);

/** Pass nullptr while the time is unknown. */
esp_err_t set_time(const char *text);

esp_err_t set_links(bool wifi, bool mqtt);

/** Once the phone has been recognised at least once, this also gates which pages appear. */
esp_err_t set_presence(bool has_key, bool present, bool ever_seen);

esp_err_t set_battery(bool present, int percent, bool charging);

/** Before init(): the diagnostics view's cards, in order, kept by pointer. */
void set_cards(const Card *cards, int count);

/** A card's summary and whether it is doing its job, which is not the same as
 *  reporting what you hoped: a phone correctly seen to be away is healthy.
 *  Strings are copied. */
esp_err_t set_card(int card, const char *summary, Level level);

/** One row of a card, by its place in Card::rows. The level colours the value;
 *  null shows "--". Strings are copied. */
esp_err_t set_row(int card, int row, const char *value, Level level = Level::Neutral);

/** The readings Setup's Diagnostics tile shows at a glance, each a few
 *  characters; null shows "--". */
enum class Glance : std::uint8_t { Uptime, Wifi, DeskLink };
inline constexpr int kGlanceCount = 3;
esp_err_t set_glance(Glance which, const char *value);


/** Re-reads the calendar and redraws its page. Thread-safe. */
esp_err_t set_calendar();

/** True while the Setup page is showing. Nothing needs pushing when it is not. */
bool diagnostics_open();

/** Shows a setting's position, and applies the ones the panel owns itself. */
esp_err_t set_setting(Setting setting, bool on);

/** Home Assistant turning the screen on or off, so the panel's own control
 *  shows the same thing. */
esp_err_t set_screen(bool on);

esp_err_t set_notification_volume(int percent);

/** The scope redraws from this; between readings it is left alone. */
esp_err_t set_radar(const radar::Snapshot &snapshot);

/** Ignored unless that aircraft is still the one selected. */
esp_err_t set_radar_details(const char *hex, const radar::Details &details);

/** RGB565. Null clears the frame. Ignored unless still selected. */
esp_err_t set_radar_photo(const char *hex, const void *pixels, int width, int height);

esp_err_t set_focus(const Focus &focus);

esp_err_t set_update(const UpdateState &state);


/** Queued rather than shown at once: they arrive in bursts. A full queue drops the oldest.
 *  `source` says where it came from, over the title, which is the headline; the
 *  message goes under it, or is the headline itself when there is no title.
 *  A negative timeout keeps it until it is tapped. */
esp_err_t notify(const char *source, const char *title, const char *message, Level level,
                 int timeout_ms);

/** A development build's: how long the radar takes to open fullscreen, draw,
 *  zoom and close, written to `out`. With the LVGL lock. */
int bench_radar(char *out, std::size_t size);

/** A development build's: the radar shown for bench_radar, with the phone away
 *  too, and whether its feed has answered yet. With the LVGL lock. */
int bench_radar_open(char *out, std::size_t size);

/** A development build's: the radar left open over the whole screen, running
 *  as it does, following the nearest aircraft; or, with `open` false, the
 *  screen put back as it was before. */
int bench_radar_full(char *out, std::size_t size, bool open);
int bench_radar_zoom_frame(char *out, std::size_t size);  // a zoom's frame, held until put back

}  // namespace ui
