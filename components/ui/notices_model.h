#pragma once

#include "ui_internal.h"

// The notices waiting to be shown, oldest first: the one on show has left the
// queue. The updates add to it and the notice card takes from it, each telling
// Topic::Notices. On the LVGL task only.
namespace ui::detail {

struct NoticesState {
    Notice waiting[NOTIFY_QUEUE_LEN]{};
    int    count = 0;
};

const NoticesState &notices_state();

/** Queued; the oldest goes when it is full. */
void notices_take(const Notice &notice);
/** The oldest waiting, out of the queue, or false with none. */
bool notices_take_next(Notice &notice);

}  // namespace ui::detail
