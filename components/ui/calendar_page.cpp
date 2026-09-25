#include "calendar_page.h"

#include "ui_internal.h"

#include "fonts/units_font.h"
#include "ical.h"
#include "icons.h"
#include "theme.h"
#include "travel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>

namespace ui {
namespace {

namespace space = theme::space;

// The overview (next event, the way there, the rest of the week) and the week view.

constexpr int AHEAD       = 40;  // events read per refresh
constexpr int LIST_ITEMS  = 40;  // day names and events in the overview's list
constexpr int WEEK_EVENTS = 48;  // blocks in the week
constexpr int WEEK_DAYS   = 7;
constexpr int HOURS_MAX   = 24;
constexpr int POINTS      = travel::kLegsMax + 1;

constexpr std::int32_t LIST_W   = 410;
constexpr std::int32_t DOT      = 10;
constexpr std::int32_t TIME_W   = 64;
constexpr std::int32_t BAR      = 4;
constexpr std::int32_t EVENT_H  = 170;  // the event card, until it has been measured
constexpr std::int32_t STOP_T_W = 64;   // times down the left of the route
constexpr std::int32_t NODE     = 16;
constexpr std::int32_t RAIL     = 4;
constexpr std::int32_t PITCH    = 84;   // between stops, at most

constexpr std::uint32_t INK_LECTURES   = 0x74c97a;
constexpr std::uint32_t INK_PRACTICALS = 0x63a9e8;
constexpr std::uint32_t INK_EXAMS      = 0xb07ce0;
constexpr std::uint32_t INK_OTHER      = 0xb0805a;
constexpr std::uint32_t INK_WORK       = 0xf0923c;


std::uint32_t feed_ink(std::uint8_t feed)
{
    switch (feed) {
        case 0: return INK_LECTURES;
        case 1: return INK_PRACTICALS;
        case 2: return INK_EXAMS;
        case ical::kWorkFeed: return INK_WORK;
        default: return INK_OTHER;
    }
}

const char *feed_kind(std::uint8_t feed)
{
    switch (feed) {
        case 0: return "Lecture";
        case 1: return "Practical";
        case 2: return "Exam";
        case ical::kWorkFeed: return "Work";
        default: return "Other";
    }
}

struct Item {  // one entry of the overview list: a day's name, or an event
    lv_obj_t *root  = nullptr;
    lv_obj_t *dot   = nullptr;
    lv_obj_t *time  = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *place = nullptr;  // under the title
};

struct Stop {  // a place on the route, on the line
    lv_obj_t *node = nullptr;
    lv_obj_t *when = nullptr;
    lv_obj_t *name = nullptr;
    lv_obj_t *note = nullptr;  // the time to change, or to spare at the end
};

struct Ride {  // the line between two stops, and how it is travelled
    lv_obj_t *rail = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *what = nullptr;
};

struct Block {  // one event in the week
    lv_obj_t *root  = nullptr;
    lv_obj_t *title = nullptr;
};

lv_obj_t *s_overview = nullptr;
lv_obj_t *s_agenda   = nullptr;
lv_obj_t *s_toggle   = nullptr;
bool      s_detailed = false;

lv_obj_t *s_kind_dot  = nullptr;
lv_obj_t *s_kind      = nullptr;
lv_obj_t *s_title     = nullptr;
lv_obj_t *s_meta      = nullptr;
lv_obj_t *s_big_name  = nullptr;
lv_obj_t *s_big       = nullptr;
lv_obj_t *s_big_note  = nullptr;
lv_obj_t *s_side_name = nullptr;
lv_obj_t *s_side      = nullptr;
lv_obj_t *s_next      = nullptr;
lv_obj_t *s_numbers   = nullptr;
lv_obj_t *s_spread    = nullptr;
lv_obj_t *s_journey   = nullptr;
lv_obj_t *s_after     = nullptr;  // the rest of the week, where the route is when there is one
lv_obj_t *s_route     = nullptr;
Stop      s_stop[POINTS];
Ride      s_ride[travel::kLegsMax];
std::int32_t s_page_h  = 0;
std::int32_t s_route_h = 0;

struct Choice {  // one of the ways there, to pick between
    lv_obj_t *root = nullptr;
    lv_obj_t *time = nullptr;
    lv_obj_t *via  = nullptr;
};
Choice       s_choice[travel::kOptionsMax];
lv_obj_t    *s_choices    = nullptr;
lv_obj_t    *s_route_name = nullptr;  // over the route when there is nothing to pick
constexpr std::int32_t CHOICE_H = 64;

constexpr int DAY_BLOCKS = 12;
constexpr int DAY_TICKS  = 13;
constexpr std::int32_t DAY_LANE = 64;
struct DayBlock {
    lv_obj_t *root  = nullptr;
    lv_obj_t *time  = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *place = nullptr;
};
std::int32_t s_day_lane_h = DAY_LANE;  // as tall as the card has room for
bool         s_day_two    = false;
lv_obj_t *s_day       = nullptr;
lv_obj_t *s_day_name  = nullptr;
lv_obj_t *s_day_track = nullptr;
lv_obj_t *s_day_now   = nullptr;
lv_obj_t *s_day_tick[DAY_TICKS];
DayBlock  s_day_block[DAY_BLOCKS];
std::int32_t s_day_w = 0;

lv_obj_t *s_list = nullptr;
Item      s_item[LIST_ITEMS];

lv_obj_t   *s_week_grid = nullptr;
lv_obj_t   *s_day_lane[WEEK_DAYS];
lv_obj_t   *s_week_prev = nullptr;
lv_obj_t   *s_week_next = nullptr;
lv_obj_t   *s_week_back = nullptr;  // to this week, when looking at another
int         s_week_shift = 0;  // weeks away from the one worth looking at
lv_obj_t   *s_week_name = nullptr;
lv_obj_t   *s_week_now  = nullptr;
lv_obj_t   *s_day_head[WEEK_DAYS];
lv_obj_t   *s_hour_mark[HOURS_MAX + 1];
lv_obj_t   *s_hour_line[HOURS_MAX + 1];
Block       s_block[WEEK_EVENTS];
std::int32_t s_grid_w = 0;
std::int32_t s_grid_h = 0;

std::int32_t s_title_w = 0;
std::int32_t s_item_w  = 0;  // an overview entry's title

void quiet(lv_obj_t *obj)
{
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
}

lv_obj_t *bare(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    theme::style_panel(obj, theme::panel, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    quiet(obj);
    return obj;
}

lv_obj_t *column_of(lv_obj_t *parent, std::int32_t gap)
{
    lv_obj_t *column = bare(parent);
    lv_obj_set_width(column, LV_PCT(100));
    lv_obj_set_height(column, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, gap, 0);
    return column;
}

lv_obj_t *row_of(lv_obj_t *parent, std::int32_t height, std::int32_t gap)
{
    lv_obj_t *row = bare(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, 0);
    return row;
}

lv_obj_t *line_label(lv_obj_t *parent, std::uint32_t colour, const lv_font_t *font)
{
    lv_obj_t *label = theme::make_label(parent, "", colour, font);
    lv_obj_set_height(label, font->line_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    quiet(label);
    return label;
}

// Dots only shorten a label of fixed height, so a title allowed two lines is
// measured and given one or two.
void fit_lines(lv_obj_t *label, std::int32_t width, int lines)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t       size{};
    lv_text_get_size(&size, lv_label_get_text(label), font, 0, 0, width, LV_TEXT_FLAG_NONE);
    lv_obj_set_height(label, std::min<std::int32_t>(size.y, lines * font->line_height));
}

lv_obj_t *dot_of(lv_obj_t *parent)
{
    lv_obj_t *dot = bare(parent);
    lv_obj_set_size(dot, DOT, DOT);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    return dot;
}

std::tm local(std::int64_t at)
{
    std::tm    when{};
    const auto stamp = static_cast<std::time_t>(at);
    localtime_r(&stamp, &when);
    return when;
}

void clock_of(std::int64_t at, char *out, std::size_t size)
{
    const std::tm when = local(at);
    std::snprintf(out, size, "%02d:%02d", when.tm_hour, when.tm_min);
}

void span_of(std::int64_t from_now, char *out, std::size_t size)
{
    const int minutes = static_cast<int>((from_now + 59) / 60);
    if (minutes <= 0) {
        std::snprintf(out, size, "now");
    } else if (minutes < 60) {
        std::snprintf(out, size, "in %d min", minutes);
    } else {
        std::snprintf(out, size, "in %dh %02dm", minutes / 60, minutes % 60);
    }
}

// Days counted from today, by the calendar rather than by 24-hour steps.
int days_from(std::int64_t now, std::int64_t at)
{
    std::tm today = local(now);
    std::tm then  = local(at);
    today.tm_hour = then.tm_hour = 12;
    today.tm_min = then.tm_min = today.tm_sec = then.tm_sec = 0;
    // Rounded: across a change of clocks the gap is an hour off a whole day.
    return static_cast<int>(std::lround(std::difftime(std::mktime(&then), std::mktime(&today)) / 86400.0));
}

void day_name(std::int64_t now, std::int64_t at, char *out, std::size_t size)
{
    static const char *const WEEKDAYS[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                           "Thursday", "Friday", "Saturday"};
    const int away = days_from(now, at);
    if (away <= 0) {
        std::snprintf(out, size, "Today");
    } else if (away == 1) {
        std::snprintf(out, size, "Tomorrow");
    } else {
        const std::tm when = local(at);
        if (away < 7) {
            std::snprintf(out, size, "%s", WEEKDAYS[when.tm_wday]);
        } else {
            std::strftime(out, size, "%a %d %b", &when);
        }
    }
}

// Timetables mark some rooms with asterisks ("**Online"); they mean nothing here.
const char *place_of(const ical::Event &event)
{
    const char *place = event.location;
    while (*place == '*' || *place == ' ') {
        ++place;
    }
    return place;
}

const travel::Option *s_going = nullptr;

// NS abbreviates its train types; spelled out, they say what kind of train.
const char *line_name(const char *line)
{
    if (std::strcmp(line, "SPR") == 0) {
        return "Sprinter";
    }
    if (std::strcmp(line, "IC") == 0) {
        return "Intercity";
    }
    return line;
}

void mode_of(const travel::Leg &leg, char *out, std::size_t size)
{
    const bool train = std::strcmp(leg.mode, "train") == 0;
    const bool walk  = std::strcmp(leg.mode, "walk") == 0;
    const bool bike  = std::strcmp(leg.mode, "bike") == 0;
    const int  mins  = static_cast<int>((leg.arrive - leg.depart + 59) / 60);
    if (walk || bike) {
        std::snprintf(out, size, "%s  \xc2\xb7  %d min", walk ? "Walk" : "Bike", mins);
    } else if (train && leg.line[0] != '\0') {
        // The backend writes a trip with changes as "SPR +1".
        char        kind[travel::kLineMax];
        const char *plus    = std::strstr(leg.line, " +");
        const int   changes = plus != nullptr ? std::atoi(plus + 2) : 0;
        std::snprintf(kind, sizeof(kind), "%.*s",
                      static_cast<int>(plus != nullptr ? plus - leg.line : std::strlen(leg.line)),
                      leg.line);
        if (changes > 0) {
            std::snprintf(out, size, "%s, %d change%s  \xc2\xb7  %d min", line_name(kind), changes,
                          changes == 1 ? "" : "s", mins);
        } else {
            std::snprintf(out, size, "%s  \xc2\xb7  %d min", line_name(kind), mins);
        }
    } else if (leg.line[0] != '\0') {
        std::snprintf(out, size, "Bus %s  \xc2\xb7  %d min", leg.line, mins);
    } else {
        std::snprintf(out, size, "%s  \xc2\xb7  %d min", train ? "Train" : "Bus", mins);
    }
}

// The ways there still worth taking, latest first as the backend answers, and
// the one on show: the best, which is the first that is not late, unless
// another was picked by hand. A pick is held by when it leaves, so it stays
// through refetches and falls away once gone. The choices run earliest first.
travel::Option s_ways[travel::kOptionsMax];
int            s_way_count   = 0;
int            s_way_order[travel::kOptionsMax];
std::int64_t   s_picked_leave = 0;

void pick_journey(bool wanted, std::int64_t now)
{
    static travel::Option options[travel::kOptionsMax];
    const int found = wanted ? travel::options(options, travel::kOptionsMax) : 0;

    s_way_count = 0;
    for (int i = 0; i < found; ++i) {
        if (options[i].leave >= now) {
            s_ways[s_way_count++] = options[i];
        }
    }
    // Hours off is nothing to act on yet.
    constexpr std::int64_t SHOW_WITHIN = 3 * 3600;
    if (s_way_count > 0 && s_ways[0].leave - now > SHOW_WITHIN) {
        s_way_count = 0;
    }

    s_going = nullptr;
    for (int i = 0; i < s_way_count && s_going == nullptr; ++i) {
        if (s_ways[i].leave == s_picked_leave) {
            s_going = &s_ways[i];
        }
    }
    if (s_going == nullptr) {
        s_picked_leave = 0;
        for (int i = 0; i < s_way_count && s_going == nullptr; ++i) {
            if (!s_ways[i].late) {
                s_going = &s_ways[i];
            }
        }
        if (s_going == nullptr && s_way_count > 0) {
            s_going = &s_ways[0];
        }
    }

    for (int i = 0; i < s_way_count; ++i) {
        int at = i;
        for (; at > 0 && s_ways[s_way_order[at - 1]].leave > s_ways[i].leave; --at) {
            s_way_order[at] = s_way_order[at - 1];
        }
        s_way_order[at] = i;
    }
}

// Where the train takes you, which is what tells one way from another: the
// station a bus goes on from, or the one you walk from.
const char *via_of(const travel::Option &way)
{
    for (int i = 0; i < way.leg_count; ++i) {
        if (std::strcmp(way.legs[i].mode, "train") == 0) {
            return way.legs[i].to;
        }
    }
    return way.leg_count > 0 ? way.legs[0].to : "";
}

// Without a route the event card keeps the column, its lines centred rather than
// spread to the corners.
bool         s_day_shown = false;
std::int64_t s_day_until = 0;  // the end of the day the strip shows
bool s_day_relayout = false;  // the strip needs drawing again at the card's size

void show_day(const ical::Event *next, std::int64_t now)
{
    s_day_shown = false;
    if (next == nullptr) {
        return;
    }
    std::tm day = local(next->start);
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    const std::int64_t from = static_cast<std::int64_t>(std::mktime(&day));
    day.tm_mday += 1;
    day.tm_isdst = -1;
    const std::int64_t to = static_cast<std::int64_t>(std::mktime(&day));

    static ical::Event events[DAY_BLOCKS];
    const int count = ical::between(from, to, events, DAY_BLOCKS);
    if (count == 0) {
        return;
    }
    s_day_shown = true;
    s_day_until = to;

    {
        char from_at[16];
        char to_at[16];
        clock_of(events[0].start, from_at, sizeof(from_at));
        std::int64_t latest = events[0].end;
        for (int i = 1; i < count; ++i) {
            latest = std::max(latest, events[i].end);
        }
        clock_of(latest, to_at, sizeof(to_at));
        char summary[64];
        std::snprintf(summary, sizeof(summary), "%d %s  \xc2\xb7  %s \xe2\x80\x93 %s", count,
                      count == 1 ? "event" : "events", from_at, to_at);
        theme::set_text(s_day_name, summary);
    }

    auto hour_at = [&](std::int64_t at) {
        return static_cast<float>(std::clamp<std::int64_t>(at - from, 0, 86400)) / 3600.0f;
    };
    float first = 8.0f;
    float last  = 18.0f;
    for (int i = 0; i < count; ++i) {
        first = std::min(first, std::floor(hour_at(events[i].start)));
        last  = std::max(last, std::ceil(hour_at(events[i].end)));
    }
    const float span = std::max(1.0f, last - first);
    const auto  x_of = [&](std::int64_t at) {
        return static_cast<std::int32_t>((hour_at(at) - first) / span * static_cast<float>(s_day_w));
    };

    const int step = span > 8.0f ? 2 : 1;
    int       tick = 0;
    char      text[40];
    for (int h = static_cast<int>(first); h <= static_cast<int>(last) && tick < DAY_TICKS; h += step) {
        std::snprintf(text, sizeof(text), "%02d", h);
        theme::set_text(s_day_tick[tick], text);
        const auto at = static_cast<std::int32_t>((static_cast<float>(h) - first) / span *
                                                  static_cast<float>(s_day_w));
        lv_obj_set_pos(s_day_tick[tick], std::clamp<std::int32_t>(at - 8, 0, s_day_w - 20), 0);
        lv_obj_set_hidden(s_day_tick[tick++], false);
    }
    for (; tick < DAY_TICKS; ++tick) {
        lv_obj_set_hidden(s_day_tick[tick], true);
    }

    const std::int32_t lanes_y = theme::type_label()->line_height + space::s;
    std::int64_t       lane_end[2] = {0, 0};
    for (int i = 0; i < DAY_BLOCKS; ++i) {
        DayBlock &block = s_day_block[i];
        if (i >= count) {
            lv_obj_set_hidden(block.root, true);
            continue;
        }
        const ical::Event &event = events[i];
        const int lane = event.start >= lane_end[0] ? 0 : 1;
        lane_end[lane] = std::max(lane_end[lane], event.end);

        const std::int32_t x = x_of(event.start);
        const std::int32_t w = std::max<std::int32_t>(x_of(event.end) - x - 3, 10);
        lv_obj_set_pos(block.root, x, lanes_y + lane * (s_day_lane_h + space::s));
        lv_obj_set_size(block.root, w, s_day_lane_h);

        const bool     coming = event.start == next->start && std::strcmp(event.summary, next->summary) == 0;
        const bool     over   = event.end < now;
        const auto     ink    = feed_ink(event.feed);
        lv_obj_set_style_bg_color(block.root,
                                  lv_color_mix(lv_color_hex(ink), lv_color_hex(theme::panel),
                                               coming ? 150 : 90),
                                  0);
        lv_obj_set_style_border_color(block.root, lv_color_hex(ink), 0);
        lv_obj_set_style_opa(block.root, over ? LV_OPA_50 : LV_OPA_COVER, 0);

        clock_of(event.start, text, sizeof(text));
        theme::set_text(block.time, text);
        theme::set_text(block.title, event.summary);
        theme::set_text(block.place, place_of(event));
        lv_obj_set_width(block.time, w - 12);
        lv_obj_set_width(block.title, w - 12);
        lv_obj_set_width(block.place, w - 12);
        // A title in a sliver breaks every word apart; the time is enough there.
        const std::int32_t room  = s_day_lane_h - 12 - theme::type_label()->line_height;
        const bool         wide  = w >= 48;
        const bool         words = w >= 96;
        lv_obj_set_hidden(block.time, !wide);
        lv_obj_set_hidden(block.title, !words || room < theme::type_label()->line_height);
        fit_lines(block.title, w - 12,
                  std::max<std::int32_t>(1, room / theme::type_label()->line_height - 1));
        lv_obj_set_hidden(block.place, !words || place_of(event)[0] == '\0' ||
                                           room < 2 * theme::type_label()->line_height);
        lv_obj_set_hidden(block.root, false);
    }
    const bool two = lane_end[1] != 0;
    s_day_two      = two;
    lv_obj_set_height(s_day_track,
                      lanes_y + s_day_lane_h * (two ? 2 : 1) + (two ? space::s : 0));

    const bool today = now >= from && now < to && hour_at(now) >= first && hour_at(now) <= last;
    lv_obj_set_hidden(s_day_now, !today);
    if (today) {
        lv_obj_set_pos(s_day_now, x_of(now) - 1, lanes_y);
        lv_obj_set_height(s_day_now, lv_obj_get_height(s_day_track) - lanes_y);
    }
}

// The next event keeps the left, with the rest of its day under it when there
// is one. The right is the way there while there is one to take, otherwise the
// rest of the week.
void place_left()
{
    const bool route = s_going != nullptr;
    const bool day   = s_day_shown;
    lv_obj_set_hidden(s_journey, !route);
    lv_obj_set_hidden(s_after, route);
    lv_obj_set_hidden(s_day, !day);
    lv_obj_set_flex_align(s_next, day ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_height(s_next, day ? LV_SIZE_CONTENT : s_page_h);
    if (!day) {
        return;
    }
    lv_obj_update_layout(s_next);
    const std::int32_t top = lv_obj_get_height(s_next) + space::m;
    lv_obj_set_y(s_day, top);
    lv_obj_set_height(s_day, s_page_h - top);
    const std::int32_t inner = s_page_h - top - 2 * space::l;
    const std::int32_t head  = theme::type_label()->line_height + space::m;
    const std::int32_t ticks = theme::type_label()->line_height + space::s;
    const int          lanes = s_day_two ? 2 : 1;
    s_day_lane_h = std::clamp<std::int32_t>(
        (inner - head - ticks - (lanes - 1) * space::s) / lanes, DAY_LANE, 132);
    lv_obj_set_y(s_day_track, head);
    s_day_relayout = true;
}

// The ways there, side by side, when there is more than one to choose from.
void show_choices(std::int64_t starts)
{
    const bool pick = s_way_count > 1;
    lv_obj_set_hidden(s_choices, !pick);
    lv_obj_set_hidden(s_route_name, pick);
    char text[16];
    for (int i = 0; i < travel::kOptionsMax; ++i) {
        const Choice &choice = s_choice[i];
        const bool    real   = pick && i < s_way_count;
        lv_obj_set_hidden(choice.root, !real);
        if (!real) {
            continue;
        }
        const travel::Option &way = s_ways[s_way_order[i]];
        const bool            on  = &way == s_going;
        lv_obj_set_state(choice.root, LV_STATE_CHECKED, on);
        clock_of(way.leave, text, sizeof(text));
        theme::set_text(choice.time, text);
        theme::set_text_color(choice.time, way.cancelled ? theme::red : theme::text);
        if (way.late && starts > 0) {
            std::snprintf(text, sizeof(text), "%d min late",
                          static_cast<int>((way.arrive - starts + 59) / 60));
            theme::set_text(choice.via, text);
        } else {
            theme::set_text(choice.via, via_of(way));
        }
        theme::set_text_color(choice.via, on          ? theme::text
                                          : way.late ? theme::amber
                                                     : theme::secondary);
    }

    // The route takes what is left of the card, clear of the corner chip.
    const std::int32_t head = pick ? CHOICE_H + space::l
                                   : theme::type_label()->line_height + space::l;
    const std::int32_t foot = theme::chip::size + theme::chip::inset - space::l + space::s;
    lv_obj_set_y(s_route, head);
    s_route_h = s_page_h - 2 * space::l - head - foot;
    lv_obj_set_height(s_route, s_route_h);
}

void show_journey(std::int64_t now, std::int64_t starts)
{
    if (s_going == nullptr) {
        return;
    }

    (void)now;
    show_choices(starts);

    char               text[64];
    const int          legs  = s_going->leg_count;
    const std::int32_t line  = theme::type_body()->line_height;
    const std::int32_t pitch = std::min<std::int32_t>(
        PITCH, (s_route_h - line - theme::type_label()->line_height) / std::max(legs, 1));
    const std::int32_t rail_x = STOP_T_W + space::s + NODE / 2;
    const std::int32_t text_x = rail_x + NODE / 2 + space::m;

    for (int i = 0; i < POINTS; ++i) {
        const Stop &stop = s_stop[i];
        const bool  real = i <= legs;
        lv_obj_set_hidden(stop.node, !real);
        lv_obj_set_hidden(stop.when, !real);
        lv_obj_set_hidden(stop.name, !real);
        lv_obj_set_hidden(stop.note, !real);
        if (!real) {
            continue;
        }
        const std::int32_t y      = i * pitch;
        const bool         leaves = i < legs;
        const bool         ends   = i == 0 || i == legs;
        const bool cancelled      = leaves && s_going->legs[i].cancelled;
        bool       late           = false;

        lv_obj_set_pos(stop.node, rail_x - NODE / 2, y + (line - NODE) / 2);
        lv_obj_set_style_bg_color(stop.node, lv_color_hex(ends ? theme::secondary : theme::panel_light),
                                  0);

        clock_of(leaves ? s_going->legs[i].depart : s_going->legs[legs - 1].arrive, text,
                 sizeof(text));
        theme::set_text(stop.when, text);
        theme::set_text_color(stop.when, cancelled ? theme::red : theme::text);
        lv_obj_set_pos(stop.when, 0, y);

        theme::set_text(stop.name, leaves ? s_going->legs[i].from : s_going->legs[legs - 1].to);
        lv_obj_set_pos(stop.name, text_x, y);
        // Walking or riding onto a train is not a change.
        const char *before = i > 0 ? s_going->legs[i - 1].mode : "";
        if (i > 0 && leaves && std::strcmp(before, "walk") != 0 &&
            std::strcmp(before, "bike") != 0) {
            const int wait =
                static_cast<int>((s_going->legs[i].depart - s_going->legs[i - 1].arrive) / 60);
            std::snprintf(text, sizeof(text), "change, %d min", wait);
        } else if (!leaves && starts > 0) {
            const int spare = static_cast<int>((starts - s_going->legs[legs - 1].arrive) / 60);
            if (spare >= 0) {
                std::snprintf(text, sizeof(text), "%d min before it starts", spare);
            } else {
                std::snprintf(text, sizeof(text), "%d min late", -spare);
            }
            late = spare < 0;
        } else {
            text[0] = '\0';
        }
        theme::set_text(stop.note, text);
        theme::set_text_color(stop.note, late ? theme::amber : theme::secondary);
        lv_obj_set_pos(stop.note, text_x, y + line);
    }

    for (int i = 0; i < travel::kLegsMax; ++i) {
        const Ride &ride = s_ride[i];
        const bool  real = i < legs;
        lv_obj_set_hidden(ride.rail, !real);
        lv_obj_set_hidden(ride.icon, !real);
        lv_obj_set_hidden(ride.what, !real);
        if (!real) {
            continue;
        }
        const travel::Leg &leg = s_going->legs[i];
        const std::int32_t top = i * pitch + line / 2;
        // Walking and cycling get the thin line maps use for them.
        const bool walk = std::strcmp(leg.mode, "walk") == 0;
        const bool bike = std::strcmp(leg.mode, "bike") == 0;
        const bool own  = walk || bike;
        lv_obj_set_pos(ride.rail, rail_x - (own ? 1 : RAIL / 2), top);
        lv_obj_set_size(ride.rail, own ? 2 : RAIL, pitch);
        lv_obj_set_style_bg_color(ride.rail, lv_color_hex(leg.cancelled ? theme::red : theme::panel),
                                  0);

        const bool         noted = lv_label_get_text(s_stop[i].note)[0] != '\0';
        const std::int32_t mid =
            top + ((noted ? theme::type_label()->line_height : 0) + pitch) / 2;
        lv_image_set_src(ride.icon, walk                                  ? &icons::walk_icon
                                    : bike                                  ? &icons::bike_icon
                                    : std::strcmp(leg.mode, "train") == 0   ? &icons::train_icon
                                                                            : &icons::bus_icon);
        lv_obj_set_style_image_recolor(ride.icon,
                                       lv_color_hex(leg.cancelled ? theme::red : theme::secondary), 0);
        lv_obj_set_pos(ride.icon, text_x, mid - icons::bus_icon.header.h / 2);

        mode_of(leg, text, sizeof(text));
        if (leg.cancelled) {
            std::strncat(text, "  \xc2\xb7  cancelled", sizeof(text) - std::strlen(text) - 1);
        }
        theme::set_text(ride.what, text);
        theme::set_text_color(ride.what, leg.cancelled ? theme::red : theme::secondary);
        lv_obj_set_pos(ride.what, text_x + static_cast<std::int32_t>(icons::bus_icon.header.w) + space::s,
                       mid - theme::type_label()->line_height / 2);
    }
}

void show_next(const ical::Event *first, std::int64_t now)
{
    char text[96];
    if (first == nullptr) {
        lv_obj_set_hidden(s_kind_dot, true);
        theme::set_text(s_kind, "");
        theme::set_text(s_title, "Nothing coming up");
        theme::set_text(s_meta, "");
        theme::set_text(s_big_name, "");
        theme::set_text(s_big, "");
        theme::set_text(s_big_note, "");
        theme::set_text(s_side, "");
        return;
    }

    const bool ongoing = first->start <= now;
    lv_obj_set_hidden(s_kind_dot, false);
    lv_obj_set_style_bg_color(s_kind_dot, lv_color_hex(feed_ink(first->feed)), 0);
    if (ongoing) {
        std::snprintf(text, sizeof(text), "%s, now", feed_kind(first->feed));
    } else {
        char day[24];
        day_name(now, first->start, day, sizeof(day));
        std::snprintf(text, sizeof(text), "%s, %s", feed_kind(first->feed), day);
    }
    theme::set_text(s_kind, text);
    theme::set_text(s_title, first->summary);
    fit_lines(s_title, s_title_w, 2);

    char from[16];
    char to[16];
    clock_of(first->start, from, sizeof(from));
    clock_of(first->end, to, sizeof(to));
    if (place_of(*first)[0] != '\0') {
        std::snprintf(text, sizeof(text), "%s \xe2\x80\x93 %s  \xc2\xb7  %s", from, to,
                      place_of(*first));
    } else {
        std::snprintf(text, sizeof(text), "%s \xe2\x80\x93 %s", from, to);
    }
    theme::set_text(s_meta, text);

    // The big number is what to act on: leaving if there is a route, otherwise the
    // start, or while it runs, the end.
    char span[32];
    if (s_going != nullptr && !ongoing) {
        clock_of(s_going->leave, text, sizeof(text));
        span_of(s_going->leave - now, span, sizeof(span));
        theme::set_text(s_big_name, s_going->cancelled ? "Leave, cancelled" : "Leave");
        theme::set_text(s_big, text);
        theme::set_text_color(s_big, s_going->cancelled ? theme::red : theme::text);
        theme::set_text(s_big_note, span);
        std::snprintf(text, sizeof(text), "starts %s", from);
        theme::set_text(s_side, text);
    } else if (ongoing) {
        span_of(first->end - now, span, sizeof(span));
        theme::set_text(s_big_name, "Ends");
        theme::set_text(s_big, to);
        theme::set_text_color(s_big, theme::text);
        theme::set_text(s_big_note, span);
        theme::set_text(s_side, "");
    } else {
        span_of(first->start - now, span, sizeof(span));
        theme::set_text(s_big_name, "Starts");
        theme::set_text(s_big, from);
        theme::set_text_color(s_big, theme::text);
        theme::set_text(s_big_note, span);
        theme::set_text(s_side, "");
    }
}

std::int64_t week_start(std::int64_t now, int weeks = 0);

void show_list(const ical::Event *ahead, int count, std::int64_t now)
{
    const std::int64_t week_end = week_start(now, 1);
    int used     = 0;
    int last_day = -1;
    // While the strip shows the next event's day, the list starts after it.
    const std::int64_t skip_until = s_going == nullptr && s_day_shown ? s_day_until : 0;
    for (int i = 1; i < count && used < LIST_ITEMS && ahead[i].start < week_end; ++i) {
        if (ahead[i].start < skip_until) {
            continue;
        }
        const int day = days_from(now, ahead[i].start);
        if (day != last_day) {
            if (used + 1 >= LIST_ITEMS) {
                break;  // a day's name with nothing under it
            }
            Item &head = s_item[used++];
            char  name[24];
            day_name(now, ahead[i].start, name, sizeof(name));
            lv_obj_set_hidden(head.dot, true);
            lv_obj_set_hidden(head.time, true);
            theme::set_text(head.title, name);
            theme::set_text_color(head.title, theme::secondary);
            lv_obj_set_style_text_font(head.title, theme::type_label(), 0);
            lv_obj_set_height(head.title, theme::type_label()->line_height);
            lv_obj_set_hidden(head.place, true);
            lv_obj_set_style_margin_top(head.root, used > 1 ? space::s : 0, 0);
            lv_obj_set_hidden(head.root, false);
            last_day = day;
        }
        Item &item = s_item[used++];
        char  clock[16];
        clock_of(ahead[i].start, clock, sizeof(clock));
        lv_obj_set_hidden(item.dot, false);
        lv_obj_set_style_bg_color(item.dot, lv_color_hex(feed_ink(ahead[i].feed)), 0);
        lv_obj_set_hidden(item.time, false);
        theme::set_text(item.time, clock);
        theme::set_text(item.title, ahead[i].summary);
        theme::set_text_color(item.title, theme::text);
        lv_obj_set_style_text_font(item.title, theme::type_body(), 0);
        fit_lines(item.title, s_item_w, 2);
        theme::set_text(item.place, place_of(ahead[i]));
        lv_obj_set_hidden(item.place, place_of(ahead[i])[0] == '\0');
        lv_obj_set_style_margin_top(item.root, 0, 0);
        lv_obj_set_hidden(item.root, false);
    }
    if (used == 0) {
        Item &none = s_item[used++];
        lv_obj_set_hidden(none.dot, true);
        lv_obj_set_hidden(none.time, true);
        theme::set_text(none.title, "Nothing else this week");
        theme::set_text_color(none.title, theme::secondary);
        lv_obj_set_style_text_font(none.title, theme::type_body(), 0);
        lv_obj_set_height(none.title, theme::type_body()->line_height);
        lv_obj_set_hidden(none.place, true);
        lv_obj_set_hidden(none.root, false);
    }
    for (int i = used; i < LIST_ITEMS; ++i) {
        lv_obj_set_hidden(s_item[i].root, true);
    }

    lv_obj_update_layout(s_list);
}

// Monday of the week worth looking at: from Saturday on, the next one. Weeks are
// counted in calendar days, not seconds: the week summer time ends has an extra
// hour, and seconds from before it land on Sunday evening.
std::int64_t week_start(std::int64_t now, int weeks)
{
    std::tm day = local(now);
    day.tm_mday -= (day.tm_wday + 6) % 7;  // back to Monday
    if (day.tm_wday == 6 || day.tm_wday == 0) {
        day.tm_mday += 7;
    }
    day.tm_mday += 7 * weeks;
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&day));
}

float hour_of(std::int64_t at)
{
    const std::tm when = local(at);
    return static_cast<float>(when.tm_hour) + static_cast<float>(when.tm_min) / 60.0f;
}

void show_week(std::int64_t now)
{
    static ical::Event week[WEEK_EVENTS];
    const std::int64_t from  = week_start(now, s_week_shift);
    const int          count = ical::between(from, from + 7 * 86400 + 3600, week, WEEK_EVENTS);

    int   days  = 5;  // the weekend only when something is on it
    float first = 8.0f;
    float last  = 18.0f;
    int   day_of[WEEK_EVENTS];
    for (int i = 0; i < count; ++i) {
        day_of[i] = std::clamp(days_from(from, week[i].start), 0, WEEK_DAYS - 1);
        days      = std::max(days, day_of[i] + 1);
        first     = std::min(first, std::floor(hour_of(week[i].start)));
        const bool same_day = days_from(week[i].start, week[i].end) == 0;
        last = std::max(last, same_day ? std::ceil(hour_of(week[i].end)) : 24.0f);
    }
    const int          hours = static_cast<int>(last - first);
    const float        pph   = static_cast<float>(s_grid_h) / static_cast<float>(hours);
    const std::int32_t axis  = 40;
    const std::int32_t col_w = (s_grid_w - axis) / days;

    char text[64];
    const std::int32_t line = theme::type_label()->line_height;
    for (int h = 0; h <= HOURS_MAX; ++h) {
        const bool real = h <= hours;
        lv_obj_set_hidden(s_hour_mark[h], !real || h == hours);
        lv_obj_set_hidden(s_hour_line[h], !real);
        if (!real) {
            continue;
        }
        const auto y = static_cast<std::int32_t>(static_cast<float>(h) * pph);
        std::snprintf(text, sizeof(text), "%02d", static_cast<int>(first) + h);
        theme::set_text(s_hour_mark[h], text);
        lv_obj_set_pos(s_hour_mark[h], 0, std::max<std::int32_t>(0, y - line / 2));
        lv_obj_set_pos(s_hour_line[h], axis, y);
        lv_obj_set_width(s_hour_line[h], s_grid_w - axis);
    }

    for (int d = 0; d < WEEK_DAYS; ++d) {
        lv_obj_set_hidden(s_day_lane[d], d >= days);
        if (d < days) {
            lv_obj_set_pos(s_day_lane[d], axis + d * col_w + 2, 0);
            lv_obj_set_size(s_day_lane[d], col_w - 4, s_grid_h);
        }
    }

    static const char *const NAMES[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    const int today = days_from(from, now);  // off the grid on another week
    for (int d = 0; d < WEEK_DAYS; ++d) {
        lv_obj_set_hidden(s_day_head[d], d >= days);
        if (d >= days) {
            continue;
        }
        const std::tm date = local(from + d * 86400 + 12 * 3600);
        std::snprintf(text, sizeof(text), "%s %d", NAMES[d], date.tm_mday);
        theme::set_text(s_day_head[d], text);
        theme::set_text_color(s_day_head[d], d == today ? theme::text : theme::secondary);
        lv_obj_set_pos(s_day_head[d], axis + d * col_w + space::s, 0);
    }

    // Each event takes the first lane free at its start; a run of overlaps is as
    // narrow as its widest point.
    int lane[WEEK_EVENTS];
    int run_of[WEEK_EVENTS];
    int run_lanes[WEEK_EVENTS];
    int runs = 0;
    for (int d = 0; d < days; ++d) {
        std::int64_t lane_end[WEEK_EVENTS];
        int          lanes_open = 0;
        std::int64_t run_end    = 0;
        for (int i = 0; i < count; ++i) {
            if (day_of[i] != d) {
                continue;
            }
            if (week[i].start >= run_end) {
                run_lanes[runs++] = 0;
                lanes_open        = 0;
            }
            int l = 0;
            while (l < lanes_open && lane_end[l] > week[i].start) {
                ++l;
            }
            if (l == lanes_open) {
                ++lanes_open;
            }
            lane_end[l]           = week[i].end;
            lane[i]               = l;
            run_of[i]             = runs - 1;
            run_lanes[runs - 1]   = std::max(run_lanes[runs - 1], l + 1);
            run_end               = std::max(run_end, week[i].end);
        }
    }

    for (int i = 0; i < WEEK_EVENTS; ++i) {
        Block &block = s_block[i];
        if (i >= count) {
            lv_obj_set_hidden(block.root, true);
            continue;
        }
        const ical::Event &event = week[i];
        const int          lanes = run_lanes[run_of[i]];
        const std::int32_t lane_w = col_w / lanes;
        const float        top_h  = std::max(hour_of(event.start), first) - first;
        const float        end_h  = days_from(event.start, event.end) == 0 ? hour_of(event.end) : last;
        const auto         y      = static_cast<std::int32_t>(top_h * pph);
        const std::int32_t h =
            std::max<std::int32_t>(static_cast<std::int32_t>((end_h - first) * pph) - y - 2, line + 6);
        lv_obj_set_pos(block.root, axis + day_of[i] * col_w + lane[i] * lane_w + 4, y + 1);
        lv_obj_set_size(block.root, lane_w - 8, h);

        const std::uint32_t ink = feed_ink(event.feed);
        lv_obj_set_style_bg_color(block.root,
                                  lv_color_mix(lv_color_hex(ink), lv_color_hex(theme::panel), 105),
                                  0);
        lv_obj_set_style_border_color(block.root, lv_color_hex(ink), 0);
        lv_obj_set_style_opa(block.root, event.end < now ? LV_OPA_50 : LV_OPA_COVER, 0);

        theme::set_text(block.title, event.summary);
        const std::int32_t lines = std::max<std::int32_t>(1, (h - 6) / line);
        lv_obj_set_size(block.title, lane_w - 8 - 12, lines * line);
        lv_obj_set_hidden(block.root, false);
    }

    const bool now_shown = today >= 0 && today < days && hour_of(now) >= first && hour_of(now) <= last;
    lv_obj_set_hidden(s_week_now, !now_shown);
    if (now_shown) {
        lv_obj_set_pos(s_week_now, axis + today * col_w + 2,
                       static_cast<std::int32_t>((hour_of(now) - first) * pph) - 1);
        lv_obj_set_width(s_week_now, col_w - 4);
    }

    const std::tm monday = local(from + 12 * 3600);
    const std::tm sunday = local(from + (days - 1) * 86400 + 12 * 3600);
    char          week_no[8];
    std::strftime(week_no, sizeof(week_no), "%V", &monday);
    char span[32];
    std::strftime(span, sizeof(span), "%d %b", &sunday);
    std::snprintf(text, sizeof(text), "Week %s  \xc2\xb7  %d \xe2\x80\x93 %s", week_no,
                  monday.tm_mday, span);
    theme::set_text(s_week_name, text);
    lv_obj_set_hidden(s_week_back, s_week_shift == 0);
}

lv_obj_t *s_toggle_week  = nullptr;
lv_obj_t *s_toggle_close = nullptr;

void show_view()
{
    lv_obj_set_hidden(s_overview, s_detailed);
    lv_obj_set_hidden(s_agenda, !s_detailed);
    lv_obj_set_hidden(s_toggle_week, s_detailed);
    lv_obj_set_hidden(s_toggle_close, !s_detailed);
}

void toggle_clicked(lv_event_t *)
{
    s_detailed   = !s_detailed;
    s_week_shift = 0;
    show_view();
    show_calendar();
}

void build_overview(lv_obj_t *parent, std::int32_t width, std::int32_t height)
{
    s_overview = bare(parent);
    lv_obj_set_size(s_overview, width, height);

    lv_obj_t *next = theme::make_card(s_overview);
    s_next         = next;
    s_page_h       = height;
    lv_obj_set_pos(next, 0, 0);
    lv_obj_set_size(next, width - LIST_W - space::m, height);
    s_title_w = width - LIST_W - space::m - 2 * space::l;
    lv_obj_set_flex_flow(next, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(next, space::xs, 0);
    quiet(next);

    lv_obj_t *kind = row_of(next, theme::type_label()->line_height, space::s);
    s_kind_dot     = dot_of(kind);
    s_kind         = line_label(kind, theme::secondary, theme::type_label());

    s_title = theme::make_label(next, "", theme::text, theme::type_title());
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    quiet(s_title);

    s_meta = line_label(next, theme::secondary, theme::type_body());
    lv_obj_set_width(s_meta, LV_PCT(100));

    s_spread = bare(next);
    lv_obj_set_size(s_spread, 1, space::l);

    lv_obj_t *numbers = row_of(next, LV_SIZE_CONTENT, space::xl);
    s_numbers         = numbers;
    lv_obj_set_flex_align(numbers, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *big = column_of(numbers, 0);
    lv_obj_set_width(big, LV_SIZE_CONTENT);
    s_big_name = line_label(big, theme::secondary, theme::type_label());
    s_big      = line_label(big, theme::text, theme::type_display());
    lv_obj_t *side = column_of(numbers, 0);
    lv_obj_set_width(side, LV_SIZE_CONTENT);
    s_big_note  = line_label(side, theme::text, theme::type_value());
    s_side      = line_label(side, theme::secondary, theme::type_body());
    s_side_name = s_side;
    lv_obj_set_style_margin_bottom(side, 6, 0);  // onto the big number's baseline

    const std::int32_t left_w  = width - LIST_W - space::m;
    const std::int32_t route_w = LIST_W - 2 * space::l;
    s_journey                  = theme::make_card(s_overview);
    lv_obj_set_pos(s_journey, width - LIST_W, 0);
    lv_obj_set_size(s_journey, LIST_W, height);
    lv_obj_set_hidden(s_journey, true);
    quiet(s_journey);

    s_route_name = theme::make_eyebrow(s_journey, "THE WAY THERE");

    s_choices = row_of(s_journey, CHOICE_H, space::s);
    for (int i = 0; i < travel::kOptionsMax; ++i) {
        Choice &choice = s_choice[i];
        choice.root    = lv_button_create(s_choices);
        theme::style_button(choice.root, theme::panel);
        theme::fill_accent(choice.root, LV_STATE_CHECKED);
        lv_obj_set_height(choice.root, CHOICE_H);
        lv_obj_set_flex_grow(choice.root, 1);
        lv_obj_set_flex_flow(choice.root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(choice.root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        choice.time = line_label(choice.root, theme::text, theme::type_value());
        choice.via  = line_label(choice.root, theme::secondary, theme::type_label());
        lv_obj_set_width(choice.via, (route_w - 2 * space::s) / travel::kOptionsMax - space::s);
        lv_obj_set_style_text_align(choice.via, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(choice.via, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_add_event_cb(
            choice.root,
            [](lv_event_t *e) {
                const auto index =
                    static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
                if (index < s_way_count) {
                    s_picked_leave = s_ways[s_way_order[index]].leave;
                    show_calendar();
                }
            },
            LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
    }

    s_route = bare(s_journey);
    lv_obj_set_size(s_route, route_w, height - 2 * space::l);

    s_day = theme::make_card(s_overview);
    lv_obj_set_pos(s_day, 0, EVENT_H + space::m);
    lv_obj_set_size(s_day, left_w, height - EVENT_H - space::m);
    lv_obj_set_hidden(s_day, true);
    quiet(s_day);
    s_day_name = line_label(s_day, theme::secondary, theme::type_label());
    s_day_w    = left_w - 2 * space::l;
    s_day_track = bare(s_day);
    lv_obj_set_size(s_day_track, s_day_w, 2 * DAY_LANE);
    for (lv_obj_t *&tick : s_day_tick) {
        tick = line_label(s_day_track, theme::secondary, theme::type_label());
        lv_obj_set_width(tick, 24);
    }
    for (DayBlock &block : s_day_block) {
        block.root = bare(s_day_track);
        lv_obj_set_style_bg_opa(block.root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(block.root, theme::radius::row, 0);
        lv_obj_set_style_border_side(block.root, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(block.root, 3, 0);
        lv_obj_set_style_pad_left(block.root, 8, 0);
        lv_obj_set_style_pad_top(block.root, 6, 0);
        lv_obj_set_flex_flow(block.root, LV_FLEX_FLOW_COLUMN);
        block.time  = line_label(block.root, theme::text, theme::type_label());
        block.title = theme::make_label(block.root, "", theme::text, theme::type_label());
        lv_label_set_long_mode(block.title, LV_LABEL_LONG_MODE_DOTS);
        quiet(block.title);
        block.place = line_label(block.root, theme::secondary, theme::type_label());
        lv_obj_set_hidden(block.root, true);
    }
    s_day_now = bare(s_day_track);
    lv_obj_set_width(s_day_now, 2);
    lv_obj_set_style_bg_opa(s_day_now, LV_OPA_COVER, 0);
    theme::fill_accent(s_day_now);

    for (Ride &ride : s_ride) {  // first, so the stops sit on top of the line
        ride.rail = bare(s_route);
        lv_obj_set_size(ride.rail, RAIL, 0);
        lv_obj_set_style_bg_opa(ride.rail, LV_OPA_COVER, 0);
        ride.icon = lv_image_create(s_route);
        lv_obj_set_style_image_recolor_opa(ride.icon, LV_OPA_COVER, 0);
        quiet(ride.icon);
        ride.what = line_label(s_route, theme::secondary, theme::type_label());
        lv_obj_set_width(ride.what, route_w - STOP_T_W - 80);
    }
    for (Stop &stop : s_stop) {
        stop.node = bare(s_route);
        lv_obj_set_size(stop.node, NODE, NODE);
        lv_obj_set_style_radius(stop.node, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(stop.node, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(stop.node, 3, 0);
        lv_obj_set_style_border_color(stop.node, lv_color_hex(theme::secondary), 0);
        stop.when = line_label(s_route, theme::text, theme::type_body());
        lv_obj_set_width(stop.when, STOP_T_W);
        stop.name = line_label(s_route, theme::text, theme::type_body());
        lv_obj_set_width(stop.name, route_w - STOP_T_W - 48);
        lv_label_set_long_mode(stop.name, LV_LABEL_LONG_MODE_DOTS);
        stop.note = line_label(s_route, theme::secondary, theme::type_label());
        lv_obj_set_width(stop.note, route_w - STOP_T_W - 48);
    }

    lv_obj_t *after = theme::make_card(s_overview);
    s_after         = after;
    lv_obj_set_pos(after, width - LIST_W, 0);
    lv_obj_set_size(after, LIST_W, height);
    quiet(after);

    s_list = column_of(after, space::s);
    lv_obj_set_height(s_list, height - 2 * space::l - theme::chip::size);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollable(s_list, true);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(theme::secondary), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_list, 4, LV_PART_SCROLLBAR);
    const std::int32_t line = theme::type_body()->line_height;
    for (Item &item : s_item) {
        item.root = row_of(s_list, LV_SIZE_CONTENT, space::s);
        lv_obj_set_flex_align(item.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        item.dot = dot_of(item.root);
        lv_obj_set_style_margin_top(item.dot, (line - DOT) / 2, 0);
        item.time = line_label(item.root, theme::secondary, theme::type_body());
        lv_obj_set_width(item.time, TIME_W);
        lv_obj_t *what = column_of(item.root, 0);
        lv_obj_set_width(what, 0);
        lv_obj_set_flex_grow(what, 1);
        item.title = theme::make_label(what, "", theme::text, theme::type_body());
        lv_obj_set_width(item.title, LV_PCT(100));
        lv_label_set_long_mode(item.title, LV_LABEL_LONG_MODE_DOTS);
        quiet(item.title);
        item.place = line_label(what, theme::secondary, theme::type_label());
        lv_obj_set_width(item.place, LV_PCT(100));
        lv_obj_set_hidden(item.root, true);
    }
    s_item_w = LIST_W - 2 * space::l - DOT - TIME_W - 2 * space::s;
}

void build_week(lv_obj_t *parent, std::int32_t width, std::int32_t height)
{
    s_agenda = theme::make_card(parent);
    lv_obj_set_pos(s_agenda, 0, 0);
    lv_obj_set_size(s_agenda, width, height);
    lv_obj_set_hidden(s_agenda, true);
    quiet(s_agenda);

    const std::int32_t inner_w = width - 2 * space::l;
    const std::int32_t inner_h = height - 2 * space::l;
    const std::int32_t head_h  = theme::type_label()->line_height + space::m;
    const std::int32_t foot_h  = theme::chip::size - space::l + theme::chip::inset + space::s;

    for (lv_obj_t *&head : s_day_head) {
        head = line_label(s_agenda, theme::secondary, theme::type_label());
    }

    s_week_grid = bare(s_agenda);
    s_grid_w    = inner_w;
    s_grid_h    = inner_h - head_h - foot_h;
    lv_obj_set_pos(s_week_grid, 0, head_h);
    lv_obj_set_size(s_week_grid, s_grid_w, s_grid_h);

    for (lv_obj_t *&lane : s_day_lane) {
        lane = bare(s_week_grid);
        lv_obj_set_style_bg_color(lane, lv_color_hex(theme::panel), 0);
        lv_obj_set_style_bg_opa(lane, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lane, theme::radius::row, 0);
    }
    for (int h = 0; h <= HOURS_MAX; ++h) {
        s_hour_line[h] = bare(s_week_grid);
        lv_obj_set_height(s_hour_line[h], 1);
        lv_obj_set_style_bg_color(s_hour_line[h], lv_color_hex(theme::panel_light), 0);
        lv_obj_set_style_bg_opa(s_hour_line[h], LV_OPA_COVER, 0);
        s_hour_mark[h] = line_label(s_week_grid, theme::secondary, theme::type_label());
    }

    for (Block &block : s_block) {
        block.root = bare(s_week_grid);
        lv_obj_set_style_bg_opa(block.root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(block.root, 6, 0);
        lv_obj_set_style_border_side(block.root, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(block.root, 3, 0);
        lv_obj_set_style_pad_left(block.root, 8, 0);
        lv_obj_set_style_pad_top(block.root, 3, 0);
        lv_obj_set_hidden(block.root, true);
        block.title = theme::make_label(block.root, "", theme::text, theme::type_label());
        lv_label_set_long_mode(block.title, LV_LABEL_LONG_MODE_DOTS);
        quiet(block.title);
    }

    s_week_now = bare(s_week_grid);
    lv_obj_set_height(s_week_now, 2);
    lv_obj_set_style_bg_opa(s_week_now, LV_OPA_COVER, 0);
    theme::fill_accent(s_week_now);

    const std::int32_t chip_y = height - space::l - theme::chip::inset - theme::chip::size;
    auto week_chip = [&](std::int32_t x, const char *glyph, int step) {
        lv_obj_t *chip = theme::make_chip(s_agenda, glyph);
        lv_obj_set_pos(chip, x, chip_y);
        lv_obj_set_ext_click_area(chip, space::s);
        lv_obj_add_event_cb(
            chip,
            [](lv_event_t *e) {
                s_week_shift += static_cast<int>(
                    reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
                show_calendar();
            },
            LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(step)));
        return chip;
    };
    const std::int32_t start = theme::chip::inset - space::l;
    s_week_prev = week_chip(start, LV_SYMBOL_LEFT, -1);
    s_week_name = line_label(s_agenda, theme::text, theme::type_body());
    lv_obj_set_width(s_week_name, 260);
    lv_obj_set_style_text_align(s_week_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_week_name, start + theme::chip::size + space::s,
                   chip_y + (theme::chip::size - theme::type_body()->line_height) / 2);
    const std::int32_t next_x = start + theme::chip::size + 2 * space::s + 260;
    s_week_next               = week_chip(next_x, LV_SYMBOL_RIGHT, 1);

    s_week_back = theme::make_button(s_agenda, "This week", theme::panel, theme::type_body());
    lv_obj_set_size(s_week_back, 150, theme::chip::size);
    lv_obj_set_style_radius(s_week_back, theme::radius::pill, 0);
    theme::set_text_color(lv_obj_get_child(s_week_back, 0), theme::secondary);
    lv_obj_set_pos(s_week_back, next_x + theme::chip::size + space::m, chip_y);
    lv_obj_set_ext_click_area(s_week_back, space::s);
    lv_obj_add_event_cb(
        s_week_back,
        [](lv_event_t *) {
            s_week_shift = 0;
            show_calendar();
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_week_back, true);
}

}  // namespace

void show_calendar()
{
    // Rebuilt every thirty seconds and on every calendar fetch; nobody needs
    // that while another page is up, and opening it draws it anew.
    if (s_title == nullptr || detail::s_page != detail::CALENDAR_PAGE) {
        return;
    }

    static ical::Event ahead[AHEAD];
    const int          count = ical::upcoming(ahead, AHEAD);
    const auto         now   = static_cast<std::int64_t>(std::time(nullptr));

    // Shown once asked for, which main does well before it starts.
    const bool want = count > 0 && ahead[0].start > now && ahead[0].start - now < kJourneyAhead;

    pick_journey(want, now);
    show_next(count > 0 ? &ahead[0] : nullptr, now);
    show_day(count > 0 ? &ahead[0] : nullptr, now);
    place_left();
    if (std::exchange(s_day_relayout, false)) {
        show_day(count > 0 ? &ahead[0] : nullptr, now);
    }
    show_journey(now, count > 0 ? ahead[0].start : 0);
    if (s_detailed) {
        show_week(now);
    } else {
        show_list(ahead, count, now);
    }
}

void build_calendar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    build_overview(page, width, height);
    build_week(page, width, height);

    s_toggle       = theme::make_chip(page, "");
    s_toggle_week  = theme::make_mark(s_toggle, &icons::calendar_icon);
    s_toggle_close = theme::make_mark(s_toggle, &icons::times_icon);
    lv_obj_set_pos(s_toggle, width - theme::chip::inset - theme::chip::size,
                   height - theme::chip::inset - theme::chip::size);
    lv_obj_set_ext_click_area(s_toggle, space::m);
    lv_obj_add_event_cb(s_toggle, toggle_clicked, LV_EVENT_CLICKED, nullptr);
    show_view();

    lv_timer_create([](lv_timer_t *) { show_calendar(); }, 30000, nullptr);
    show_calendar();
}

}  // namespace ui
