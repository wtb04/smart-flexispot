#pragma once

// The design lab: every part of the kit on one screen, in every state, to judge
// how it looks, feels and reacts before any page is built from it.

namespace ui::lab {

/** Builds the lab on the active screen, in place of the panel's pages. */
void build();

/**
 * A key, as the simulator passes them on: 1 to 4 the sheets, L the look,
 * G the glass, A the accent, N a notice, B the scrim's blur.
 */
void key(int code);

}  // namespace ui::lab
