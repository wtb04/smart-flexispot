#pragma once

#include "esp_err.h"
#include "radar.h"

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

inline constexpr int kPresetCount = 4;

enum class MediaAction : std::uint8_t {
    PlayPause,
    Previous,
    Next,
    VolumeDown,
    VolumeUp,
    Mute,
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

// Settings that belong to a subsystem live in that subsystem's page rather
// than in a list of their own.
enum class Setting : std::uint8_t {
    Charging,
    PresenceGate,
    DeskBluetooth,
    Count,
};

/** The panel has already applied and redrawn it; this is for storing it. */
using SettingHandler = void (*)(Setting setting, bool on);

/** preview is set when the slider is let go, which is the moment to play
 *  something at the chosen level. */
using VolumeHandler = void (*)(int percent, bool preview);

using RestartHandler = void (*)();

/** showing is true while the scope is the page on screen, reachable is false
 *  once presence gating has hidden it away entirely. */
using RadarHandler = void (*)(bool showing, bool reachable);

// Bluetooth is the radio. Presence and the desk are two things that ride on
// it, and they fail separately from it and from each other.
enum class Subsystem : std::uint8_t {
    Network,
    HomeAssistant,
    Bluetooth,
    Presence,
    Power,
    Desk,
    System,
    Count,
};

/** Fills `out` with recent log lines for the named subsystem. Runs on the LVGL
 *  task, so it must not block. */
using LogHandler = void (*)(const char *subsystem, char *out, std::size_t size);

// Every setter here takes the LVGL lock, so any task may call them.

/** An empty title means nothing is playing; the card then shows the state. Strings are copied. */
esp_err_t set_media(const char *source, const char *title, const char *artist, const char *state,
                    bool playing);

/** RGB565, media::kArtSize square. Null hides the art; the buffer must live until it is replaced. */
// Null with placeholder set leaves an empty frame where the cover would be,
// for a cover that exists but could not be fetched; null without it gives the
// space back to the text.
esp_err_t set_album_art(const void *pixels, bool placeholder);

/** The position is carried forward while playing; a duration of zero hides the bar. */
esp_err_t set_media_progress(int position_s, int duration_s, bool playing);

esp_err_t set_media_volume(int percent);

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
};

/** Requires the LVGL port to be running. */
esp_err_t init(const Handlers &handlers, int initial_brightness);

// Advances the startup screen. Ignored once it has been dismissed.
esp_err_t splash_step(const char *label, int percent);

// Fades out the startup screen. Safe to call more than once.
esp_err_t splash_done();

/** Height in millimetres, or negative for "unknown". */
esp_err_t set_height(int height_mm);

// Highlights the preset the desk is currently standing at; a tap on it does
// nothing, though holding to store still works.
esp_err_t set_preset_active(int index, bool active);

/** Unavailable dims and disables every desk control, so nothing can be pressed in vain. */
esp_err_t set_desk_available(bool available);

/** Pass nullptr while the time is unknown. */
esp_err_t set_time(const char *text);

esp_err_t set_links(bool wifi, bool mqtt);

/** Once the phone has been recognised at least once, this also gates which pages appear. */
esp_err_t set_presence(bool has_key, bool present, bool ever_seen);

esp_err_t set_battery(bool present, int percent, bool charging);

// One labelled row on the Setup page. Whatever knows a value pushes it here and
// the page decides where it lands, so a new readout is a row in the tables in
// ui.cpp plus one call, not a layout change.
enum class Info : std::uint8_t {
    WifiState,
    WifiSsid,
    WifiIp,
    WifiMac,
    WifiSignal,
    WifiChannel,
    HaBroker,
    HaSocket,
    HaEntities,
    PhoneRadio,
    PhoneKey,
    PhoneState,
    PhoneSignal,
    BleLink,
    BleTrip,
    BleLoss,
    PowerSource,
    PowerCharge,
    PowerVolts,
    PowerCurrent,
    PowerStatus,
    DeskTransport,
    DeskLink,
    DeskHeight,
    DeskActive,
    DeskStand,
    DeskSit,
    DeskOne,
    DeskTwo,
    SysFirmware,
    SysBuilt,
    SysUptime,
    SysRam,
    SysPsram,
    SysRamLow,
    Count,
};

/** Strings are copied. The level colours the value; a null value clears the row. */
esp_err_t set_info(Info field, const char *value, Level level = Level::Neutral);

/** True while the Setup page is showing. Nothing needs pushing when it is not. */
bool diagnostics_open();

/** Shows a setting's position, and applies the ones the panel owns itself. */
esp_err_t set_setting(Setting setting, bool on);

esp_err_t set_notification_volume(int percent);

/** The scope redraws from this; between readings it is left alone. */
esp_err_t set_radar(const radar::Snapshot &snapshot);

/** Ignored unless that aircraft is still the one selected. */
esp_err_t set_radar_details(const char *hex, const radar::Details &details);

/** RGB565. Null clears the frame. Ignored unless still selected. */
esp_err_t set_radar_photo(const char *hex, const void *pixels, int width, int height);

/** Whether the subsystem is doing its job, which is not the same as whether it
 *  is reporting what you hoped: a phone correctly seen to be away is healthy. */
esp_err_t set_health(Subsystem which, Level level);

/** Queued rather than shown at once: they arrive in bursts. A full queue drops the oldest. */
esp_err_t notify(const char *title, const char *message, const char *level, int timeout_ms);

}  // namespace ui
