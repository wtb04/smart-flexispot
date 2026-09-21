#pragma once

#include "esp_err.h"

#include <cstdint>

namespace ui {

enum class Move : std::int8_t {
    Stop = 0,
    Up   = 1,
    Down = -1,
};

/**
 * @brief Invoked from the LVGL task when a button is pressed or released.
 *
 * Fires Move::Up / Move::Down on press and Move::Stop on release. Must not
 * block: it runs with the LVGL lock held.
 */
using MoveHandler = void (*)(Move direction);

/**
 * @brief Invoked from the LVGL task when a preset button is used.
 *
 * A tap sends the desk to that preset; a long press stores the current height
 * there. Must not block: it runs with the LVGL lock held.
 */
using PresetHandler = void (*)(int index, bool store);

/** Invoked from the LVGL task as the brightness slider moves. */
using BrightnessHandler = void (*)(int percent);

/** Invoked when the thermostat dial is released, with the chosen setpoint. */
using SetpointHandler = void (*)(float celsius);

/** Invoked when the thermostat's mode button is pressed. */
using ModeHandler = void (*)();

/** Number of preset buttons on screen. */
inline constexpr int kPresetCount = 4;

/** Free-form slots in the row under the lights button. */
inline constexpr int kTileCount = 4;

/** Read-only chips along the top of the home screen. */
inline constexpr int kPillCount = 4;

/** Individual lights behind the lights button's long press. */
inline constexpr int kLightCount = 4;

/** Round toggles in the thermostat card's top corner. */
inline constexpr int kDialToggleCount = 2;

/** What the thermostat is actually doing, as opposed to what it is set to. */
enum class Hvac : std::uint8_t {
    Off,
    Idle,     // on, but the setpoint is already met
    Heating,  // the boiler is running
};

/** How a reading compares with what the room should be sitting at. */
enum class Level : std::uint8_t {
    Neutral,  // no threshold, or nothing reported
    Good,
    Warn,
    Bad,
};

/** Invoked from the LVGL task when a tile that can act is tapped. */
using TileHandler = void (*)(int index);

/** Invoked when the lights button is tapped: everything on, or everything off. */
using LightsHandler = void (*)();

/** Invoked when one light in the long-press picker is tapped. */
using LightHandler = void (*)(int index);

/** Invoked when one of the thermostat card's corner toggles is tapped. */
using DialToggleHandler = void (*)(int index);

/**
 * @brief Fills one home-screen tile. Thread-safe.
 *
 * A tile with an empty label is hidden, so a screen can use fewer than all of
 * them. `on` tints it, and `actionable` decides whether tapping does anything.
 */
esp_err_t set_tile(int index, const char *label, const char *value, bool on, bool actionable);

/**
 * @brief One reading chip along the top. Thread-safe.
 *
 * An empty label hides it, and the remaining chips close up the gap.
 */
esp_err_t set_pill(int index, const char *label, const char *value, Level level);

/**
 * @brief The single large lights button. Thread-safe.
 *
 * The bulbs drawn on it come from set_light(), so both have to be pushed for
 * the button to tell the whole story.
 */
esp_err_t set_lights(const char *label, const char *state, bool on);

/**
 * @brief One light in the long-press picker. Thread-safe.
 *
 * An empty name hides that button, so fewer lights than slots is fine.
 */
esp_err_t set_light(int index, const char *name, const char *state, bool on);

/** One corner toggle on the thermostat card; an empty label hides it. Thread-safe. */
esp_err_t set_dial_toggle(int index, const char *label, bool on);

/**
 * @brief The thermostat dial on the home screen. Thread-safe.
 *
 * Pass a negative target to show it as unavailable.
 */
/** Bounds and step come from the entity, so a reconfigured thermostat follows. */
esp_err_t set_thermostat_range(float min_c, float max_c, float step_c);

/**
 * @brief The dial's readings. Thread-safe.
 *
 * Ignored while a finger is on the dial, so an update mid-drag does not yank
 * the setpoint out from under it.
 */
esp_err_t set_thermostat(float current_c, float target_c, const char *mode, Hvac state);

/** Builds the screen. Requires the LVGL port to be running. */
/** Everything the screen calls back into. */
struct Handlers {
    MoveHandler       move;
    PresetHandler     preset;
    BrightnessHandler brightness;
    TileHandler       tile;
    SetpointHandler   setpoint;
    ModeHandler       mode;
    LightsHandler     lights;
    LightHandler      light;
    DialToggleHandler dial_toggle;
};

esp_err_t init(const Handlers &handlers, int initial_brightness);

/** Height in millimetres, or negative for "unknown". Thread-safe. */
esp_err_t set_height(int height_mm);

/**
 * @brief Whether the control box is talking to us. Thread-safe.
 *
 * Unavailable dims every desk control and blanks the readout, so the panel
 * cannot be pressed in the belief it will do something.
 */
esp_err_t set_desk_available(bool available);

/** Clock in the top bar. Pass nullptr while the time is unknown. Thread-safe. */
esp_err_t set_time(const char *text);

/** Network and broker indicators. Thread-safe. */
esp_err_t set_links(bool wifi, bool mqtt);

/**
 * @brief Presence of the tracked phone. Thread-safe.
 *
 * Drives the phone glyph in the rail and, once the phone has been recognised
 * at least once, which pages the navigation offers.
 */
esp_err_t set_presence(bool has_key, bool present, bool ever_seen);

/** Battery indicator: an icon only, coloured by level. Thread-safe. */
esp_err_t set_battery(bool present, int percent, bool charging);

/**
 * @brief Queues a notification popup. Thread-safe; text is copied.
 *
 * Queued rather than shown immediately: notifications arrive in bursts, and
 * replacing the visible one loses whatever it said. A full queue drops the
 * oldest.
 */
esp_err_t notify(const char *title, const char *message, const char *level, int timeout_ms);

}  // namespace ui
