#pragma once

#include "views.h"

#include <cstdint>
#include <functional>

// What the views learn from rather than from one another: a topic is told it
// changed, and once the pending updates have been applied, everything that
// follows it is called once, however often it changed. What follows a topic
// for a view is called only while the view is open, and as it opens, with
// things as they are by then. On the LVGL task only.
namespace ui::detail {

enum class Topic : std::uint8_t {
    Second,  // the clock went on a second
    Focus,   // the focus timer: see focus_model.h
    Media,   // what plays: see media_model.h
    Count,
};

/** Calls `changed` after `topic` changes; with a view, only while it is open,
 *  and as it opens. Called once now, too. */
void subscribe(Topic topic, ViewId view, std::function<void()> changed);

/** Marks `topic` changed; its followers are called by deliver_topics(). */
void publish(Topic topic);

/** Calls the followers of every topic changed since last time. */
void deliver_topics();

/** Calls everything that follows a topic for `view`, as it opens. */
void deliver_to_view(ViewId view);

}  // namespace ui::detail
