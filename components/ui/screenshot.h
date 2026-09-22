#pragma once

namespace ui {
/** Writes a quarter-size picture of whatever is on screen to the console, as
 *  base64 RGB565 between BEGIN and END. For looking at a layout rather than
 *  guessing at it; see tools/screenshot.py. */
void screenshot();

}  // namespace ui
