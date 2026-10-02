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
    Picks,   // the favourites: see media_model.h
    Desk,    // its height, presets and whether it answers: see room_model.h
    Lights,  // which are on: see room_model.h
    Status,  // the phone, the network, the time and the screen: see status_model.h
    Home,    // the home page's pills, thermostat and toggles: see home_model.h
    Settings,  // the choices and the notification volume: see settings_model.h
    Update,    // firmware on its way or waiting: see settings_model.h
    Diagnostics,  // the panel's own report and its glances: see diagnostics_model.h
    Calendar,  // the calendar fetched again: the ical component has it
    Radar,     // the traffic fetched again: the radar component has it
    Lookup,    // a tapped aircraft's details or photo: see radar_model.h
    Notices,   // the notices waiting: see notices_model.h
    Page,      // the page on show: s_page
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
