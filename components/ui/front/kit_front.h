#pragma once

// What the parts of the design kit's screen share: kit_front.cpp puts them
// together and moves between the pages; each part is a file of its own.

#include "kit.h"
#include "topics.h"
#include "ui_core.h"

#include <cstddef>
#include <cstdint>

namespace ical {
struct Event;
}

namespace ui::kitfront {

using detail::publish;
using detail::subscribe;
using detail::Topic;

/** The pages in the order the dock lists them, which is the order they slide in. */
enum Page : int { HOME, CALENDAR, RADAR, SETUP, PAGE_COUNT };

/** The phone away with the gate on: the owner's pages, and what shows of them elsewhere, are away. */
bool owner_away();
/** Setup from the bar's status, and back to where it was opened from. */
void toggle_setup();

struct DockItems {
    lv_obj_t *home = nullptr, *calendar = nullptr, *radar = nullptr;
};

/** The desk folded out of the dock's height: every preset, up and down. kit_front_desk.cpp */
void open_desk(lv_obj_t *source);
/** The desk's card following the desk from the start, so it knows where each preset was seen. kit_front_desk.cpp */
void follow_desk();
/** The focus timer's card, from the dock's button. kit_front_views_focus.cpp */
void open_focus(lv_obj_t *source);

/** The bar's cards, each out of what opened it, and the Claude Code sessions'
 *  pill and card. kit_front_cards.cpp */
void open_media(lv_obj_t *source);
void open_lights(lv_obj_t *source);
void open_heating(lv_obj_t *source);
void build_claude(lv_obj_t *bar);

// Each part's own, in kit_front_<part>.cpp.
DockItems build_dock(lv_obj_t *chrome);  // the dock, its desk and focus, and the items that choose the pages
void      build_bar(lv_obj_t *chrome);   // the status, what plays, the room, and the cards they open
void      build_home(lv_obj_t *page);
void      build_calendar(lv_obj_t *page);
void      build_radar(lv_obj_t *page);
void      build_setup(lv_obj_t *page);
void      build_views(lv_obj_t *screen);  // the fullscreen views, the favourites, the notices

// Setup's own, in kit_front_setup*.cpp.
/** A row of Setup's: room round each, on a panel read from an arm's length. */
inline constexpr std::int32_t SETUP_ROW_H = 82;
/** What the panel reports of itself, under a caption in `column`: how it is, and
 *  each part, whose readings open on a tap and whose log on a hold, in a tile
 *  `h` tall. kit_front_setup_diag.cpp */
void build_setup_diagnostics(lv_obj_t *column, std::int32_t w, std::int32_t h);

// What Home's next event takes from the calendar's, in kit_front_calendar.cpp.
std::uint32_t event_ink(std::uint8_t feed);
/** "Lecture, in 2h 05m", "Exam, ends in 20 min", "Practical, Monday". */
void event_kind(const ical::Event &event, std::int64_t now, char *out, std::size_t size);
/** The course of "Course - Session", or the whole title. */
void event_course(const ical::Event &event, char *out, std::size_t size);
/** Where, as street and town. */
void event_place(const ical::Event &event, char *out, std::size_t size);
void event_clock(std::int64_t at, char *out, std::size_t size);

// The fullscreen views and the favourites, from the bar's cards. kit_front_views.cpp
/** The music view, or the cinema for a video; nothing while nothing plays. */
void open_now_playing();
/** The favourites to start, as a sheet, out of its source or else what the
 *  finger is on; nothing while there are none. */
void open_favourites();
void open_favourites(lv_obj_t *source);

// The radar's own, in kit_front_radar.cpp.
/** As Home's Overhead opens the radar page: that aircraft chosen, and on the scope. */
void radar_opens_on(const char *hex);

/** Album art is square, its corners only eased: a few pixels, a little more on a big one. */
constexpr std::int32_t album_radius(std::int32_t side) { return 4 + side / 40; }

// What plays, in its cover's colours, in kit_front_album.cpp.
struct AlbumColours {
    bool       vivid = false;  // false: a cover of greys, which keeps the page's colours
    lv_color_t back{}, surface{}, accent{};
    lv_color_t ink{};  // the hue as text reads on `back`: the accent on the dark, a deep shade of it on the pale
};
/** From an RGB565 cover; `dark` for the dark look's shades, the light look's paler ones otherwise. */
AlbumColours album_colours(const std::uint16_t *pixels, int count, bool dark);

/** The speaker the music plays on, as a mask `side` square, for a cover's well
 *  while nothing plays; nullptr without the memory for it. kit_front_speaker.cpp */
const lv_image_dsc_t *speaker_mark(std::int32_t side);

}  // namespace ui::kitfront
