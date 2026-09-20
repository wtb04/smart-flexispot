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

/** Number of preset buttons on screen. */
inline constexpr int kPresetCount = 4;

/** Builds the screen. Requires the LVGL port to be running. */
esp_err_t init(MoveHandler on_move, PresetHandler on_preset, BrightnessHandler on_brightness,
               int initial_brightness);

/** Height in millimetres, or negative for "unknown". Thread-safe. */
esp_err_t set_height(int height_mm);

/** Clock in the top bar. Pass nullptr while the time is unknown. Thread-safe. */
esp_err_t set_time(const char *text);

/** Network and broker indicators. Thread-safe. */
esp_err_t set_links(bool wifi, bool mqtt);

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
