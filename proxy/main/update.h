#pragma once

#include "deskproto.h"

// A new firmware arriving from the panel over the link, written to the other
// app slot as it comes and booted once all of it is there and checks out.
namespace update {
/** One step of it; 0, or the application ATT error the panel is answered with.
 *  On the NimBLE host task. */
int take(const deskproto::UpdateMessage &message);

/** Throws away whatever half an image arrived, as when the panel goes. */
void abandon();

/** True while an image is arriving, when nothing is to move the desk. */
bool running();

/** A firmware booted from an update has this long to prove itself by linking
 *  with the panel; not doing so restarts, and the bootloader returns to the one
 *  before. Call once, at start. */
void watch();

/** The panel linked: the firmware keeps its place. */
void confirm();

}  // namespace update
