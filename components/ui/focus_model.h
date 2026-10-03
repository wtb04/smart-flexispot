#pragma once

#include "ui.h"

#include <cstddef>
#include <cstdint>

// The focus timer as last told, and what follows from it, for whatever shows
// it: the fullscreen timer, its card, the dock's button, the settings and the
// badge over other views.
// Changing it publishes Topic::Focus. On the LVGL task only.
namespace ui::detail {

const Focus &focus_state();
void         set_focus_state(const Focus &focus);

std::int64_t focus_now_ms();
/** Rounded up, so a countdown reads zero only once it is over. */
std::int64_t whole_seconds_up(std::int64_t ms);

bool          focus_idle(const Focus &focus);
bool          focus_resting(const Focus &focus);
/** Set up and not yet started: the next part, waiting to be started. */
bool          focus_waiting(const Focus &focus);
bool          focus_paused(const Focus &focus);
std::int32_t  focus_left_ms(const Focus &focus);
/** Green resting, the accent working. */
std::uint32_t focus_ink(bool resting);
/** How long is left as MM:SS, or Ready while it waits. */
void          focus_clock_text(const Focus &focus, char *out, std::size_t size);

}  // namespace ui::detail
