#pragma once

#include "lvgl.h"

#include <cstdint>

// The lab's sheet of rows and readings, and what the bench does with it.
namespace ui::lab::lists {

void build(lv_obj_t *sheet);

void scroll_tick(std::uint32_t at, std::uint32_t frame);
void press_tick(std::uint32_t at, std::uint32_t frame);
void readouts_tick(std::uint32_t at, std::uint32_t frame);
void sheet_open_tick(std::uint32_t at, std::uint32_t frame);
void sheet_scroll_tick(std::uint32_t at, std::uint32_t frame);
void sheet_close_tick(std::uint32_t at, std::uint32_t frame);

/** The same long list built several ways, each scrolled and timed; prints a table when done. */
void study();
const char *report();

}  // namespace ui::lab::lists
