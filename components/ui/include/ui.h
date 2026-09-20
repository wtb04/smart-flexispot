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

/** Builds the screen. Requires the LVGL port to be running. */
esp_err_t init(MoveHandler on_move, PresetHandler on_preset);

/** Number of preset buttons on screen. */
inline constexpr int kPresetCount = 4;

/** Height in millimetres, or a negative value for "unknown". Thread-safe. */
esp_err_t set_height(int height_mm);

/** One-line status under the height. Thread-safe. Text is copied. */
esp_err_t set_status(const char *text);

}  // namespace ui
