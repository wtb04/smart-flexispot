#pragma once

#include "kit.h"

// The design lab: every part of the kit on one screen, in every state, to judge
// how it looks, feels and reacts before any page is built from it.

namespace ui::lab {

/** Builds the lab on the active screen, in place of the panel's pages; `clock` times its frames. */
void build(kit::perf::Clock clock = nullptr);

/**
 * A key, as the simulator passes them on: 1 to 5 the sheets, L the look,
 * G the glass, A the accent, N a notice, B the scrim's blur, F how popovers
 * make their glass, P the bench, which prints how long its frames took.
 */
void key(int code);

bool built();
/** Runs the bench: a fixed round of presses, popovers, needles, glass and scrolling, timed frame by frame. */
void bench();
/** The last bench's table, and how long the last popover took to freeze the screen. */
int  report(char *out, int size);

}  // namespace ui::lab
