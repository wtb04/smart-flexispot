#pragma once

// Firmware updates, played: nothing is sent or installed. U steps through what
// the rail, the Setup dot and the restart tile show while one arrives and waits.
namespace updates {
void next();

/** Once per frame, while one arrives. */
void tick();

/** Update now in Setup: whatever is ready is taken as installed. */
void install();
}  // namespace updates
