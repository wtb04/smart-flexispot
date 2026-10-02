#include "calendar_page.h"

#include "ui_internal.h"
#include "status_model.h"
#include "topics.h"

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

constexpr int SECONDS_PER_WEEK = units::kSecondsPerDay * units::kDaysPerWeek;

// Weekdays as std::tm counts them.
constexpr int SUNDAY   = 0;
constexpr int MONDAY   = 1;
constexpr int SATURDAY = 6;
// No change of clocks moves midday into another day.
constexpr int NOON = 12;

constexpr std::uint32_t REFRESH_MS = 30 * units::kMsPerSecond;

constexpr std::uint8_t FEED_LECTURES   = 0;
constexpr std::uint8_t FEED_PRACTICALS = 1;
constexpr std::uint8_t FEED_EXAMS      = 2;

constexpr std::uint32_t INK_LECTURES   = 0x74c97a;
constexpr std::uint32_t INK_PRACTICALS = 0x63a9e8;
constexpr std::uint32_t INK_EXAMS      = 0xb07ce0;
constexpr std::uint32_t INK_OTHER      = 0xb0805a;
constexpr std::uint32_t INK_WORK       = 0xf0923c;

constexpr int EVENTS_AHEAD    = 40;  // read per refresh
constexpr int LIST_ITEMS      = 40;
constexpr std::int32_t LIST_FADE_H = 28;  // day names and events in the overview's list
constexpr int WEEK_EVENTS     = 48;
constexpr int WEEK_DAYS       = units::kDaysPerWeek;
constexpr int WORKDAYS        = 5;
constexpr int HOURS_MAX       = units::kHoursPerDay;
constexpr int POINTS          = travel::kLegsMax + 1;
constexpr int TITLE_LINES_MAX = 2;

// The hours the strip and the week show however little is on them.
constexpr float WORKDAY_FIRST_HOUR = 8.0f;
constexpr float WORKDAY_LAST_HOUR  = 18.0f;
// Over this many hours, a mark an hour is a grid, so they go every other hour.
constexpr int HOURLY_MARKS_MAX = 8;
constexpr int SPARSE_MARK_STEP = 2;

constexpr std::int64_t SHOW_WAYS_WITHIN = 3 * units::kSecondsPerHour;  // hours off is nothing to act on yet

constexpr std::int32_t LIST_W        = 410;
constexpr std::int32_t DOT           = 10;
constexpr std::int32_t TIME_W        = 64;
constexpr std::int32_t EVENT_H       = 170;  // the event card, until it has been measured
constexpr std::int32_t BASELINE_LIFT = 6;    // the side numbers, onto the big number's baseline
constexpr std::int32_t SCROLLBAR_W   = 4;
constexpr std::int32_t CHOICE_H      = 64;
constexpr std::int32_t NOW_LINE_W    = 2;
// How far a corner chip reaches up into a card's padding, with a gap over it.
constexpr std::int32_t CHIP_CLEARANCE =
    theme::chip::size + theme::chip::inset - space::l + space::s;

constexpr std::int32_t STOP_TIME_W    = 64;
constexpr std::int32_t NODE           = 16;
constexpr std::int32_t NODE_BORDER_W  = 3;
constexpr std::int32_t RAIL_W         = 4;
constexpr std::int32_t PATH_W         = 2;   // walking and cycling get the thin line maps use for them
constexpr std::int32_t STOP_PITCH_MAX = 84;
constexpr std::int32_t STOP_TEXT_TRIM = 48;  // off a stop's words, for the line beside them
constexpr std::int32_t RIDE_TEXT_TRIM = 80;  // and off a ride's, for its icon too

constexpr int          DAY_BLOCKS        = 12;
constexpr int          DAY_TICKS         = 13;
constexpr int          DAY_LANES         = 2;
constexpr std::int32_t DAY_LANE_MIN_H    = 64;
constexpr std::int32_t DAY_LANE_MAX_H    = 132;
constexpr std::int32_t TICK_W            = 24;
constexpr std::int32_t TICK_LEAD         = 8;  // the digits start before their hour, to sit over it
constexpr std::int32_t TICK_DIGITS_W     = 20;
constexpr std::int32_t DAY_BLOCK_GAP     = 3;
constexpr std::int32_t DAY_BLOCK_MIN_W   = 10;
constexpr std::int32_t DAY_BLOCK_PAD_TOP = 6;
constexpr std::int32_t DAY_TIME_MIN_W    = 48;
constexpr std::int32_t DAY_TITLE_MIN_W   = 96;
// How much of the feed's colour goes into the card's.
constexpr std::uint8_t NEXT_BLOCK_MIX = 150;
constexpr std::uint8_t DAY_BLOCK_MIX  = 90;

constexpr std::int32_t BLOCK_EDGE_W     = 3;
constexpr std::int32_t BLOCK_PAD_LEFT   = 8;
constexpr std::int32_t BLOCK_TEXT_INSET = 12;  // a block's width less its words'

constexpr std::int32_t HOUR_AXIS_W           = 40;
constexpr std::int32_t TODAY_INSET           = 2;
constexpr std::int32_t DAY_HEAD_MAX_W        = 96;
constexpr std::int32_t DAY_HEAD_PAD_V        = 2;
constexpr std::int32_t WEEK_NO_W             = 48;
constexpr std::int32_t WEEK_BLOCK_PAD_TOP    = 4;
// Inset from the day's edges, so today's band shows round every event.
constexpr std::int32_t WEEK_BLOCK_INSET      = 8;
constexpr std::int32_t WEEK_BLOCK_APART      = 4;  // between events side by side
constexpr std::int32_t WEEK_BLOCK_CLEARANCE  = 1;  // off the hour lines
constexpr std::int32_t WEEK_BLOCK_MIN_SPARE  = 6;  // under the time, in the shortest block
constexpr std::int32_t WEEK_TIME_MIN_W       = 40;
constexpr std::int32_t WEEK_TITLE_MIN_W      = 72;
constexpr int          TITLE_AND_PLACE_LINES = 2;
constexpr std::uint8_t WEEK_BLOCK_MIX        = 110;
constexpr std::uint8_t PAST_BLOCK_MIX        = 40;
constexpr std::int32_t DATES_W               = 180;  // "28 Sep – 4 Oct" at the widest
constexpr std::int32_t THIS_WEEK_W           = 150;

constexpr std::int32_t DETAIL_W            = 620;
constexpr std::int32_t DETAIL_H_UNMEASURED = 240;

std::uint32_t feed_ink(std::uint8_t feed)
{
    switch (feed) {
        case FEED_LECTURES: return INK_LECTURES;
        case FEED_PRACTICALS: return INK_PRACTICALS;
        case FEED_EXAMS: return INK_EXAMS;
        case ical::kWorkFeed: return INK_WORK;
        default: return INK_OTHER;
    }
}

const char *feed_kind(std::uint8_t feed)
{
    switch (feed) {
        case FEED_LECTURES: return "Lecture";
        case FEED_PRACTICALS: return "Practical";
        case FEED_EXAMS: return "Exam";
        case ical::kWorkFeed: return "Work";
        default: return "Other";
    }
}

struct Item {  // one entry of the overview list: a day's name, or an event
    lv_obj_t *root  = nullptr;
    lv_obj_t *dot   = nullptr;
    lv_obj_t *time  = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *place = nullptr;
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

struct Block {  // one event in the strip or the week
    lv_obj_t *root  = nullptr;
    lv_obj_t *time  = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *place = nullptr;
};

lv_obj_t *s_overview = nullptr;
lv_obj_t *s_agenda   = nullptr;
bool      s_detailed = false;

lv_obj_t *s_kind_dot  = nullptr;
lv_obj_t *s_kind      = nullptr;
lv_obj_t *s_title     = nullptr;
lv_obj_t *s_meta      = nullptr;
lv_obj_t *s_big_name  = nullptr;
lv_obj_t *s_big       = nullptr;
lv_obj_t *s_big_note  = nullptr;
lv_obj_t *s_side      = nullptr;
lv_obj_t *s_next      = nullptr;
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

std::int32_t s_day_lane_h = DAY_LANE_MIN_H;  // as tall as the card has room for
bool         s_day_two    = false;
lv_obj_t *s_day       = nullptr;
lv_obj_t *s_day_track = nullptr;
lv_obj_t *s_day_now   = nullptr;
lv_obj_t *s_day_tick[DAY_TICKS];
Block     s_day_block[DAY_BLOCKS];
ical::Event s_day_event[DAY_BLOCKS];  // what each of the strip's blocks shows
std::int32_t s_day_w = 0;

lv_obj_t *s_list = nullptr;
Item      s_item[LIST_ITEMS];

lv_obj_t   *s_day_lane[WEEK_DAYS];
lv_obj_t   *s_week_back = nullptr;  // to this week, when looking at another
int         s_week_shift = 0;  // weeks away from the one worth looking at
lv_obj_t   *s_week_name = nullptr;
lv_obj_t   *s_week_no   = nullptr;  // in the corner over the hours
lv_obj_t   *s_week_now  = nullptr;
lv_obj_t   *s_day_head[WEEK_DAYS];
lv_obj_t   *s_hour_mark[HOURS_MAX + 1];
lv_obj_t   *s_hour_line[HOURS_MAX + 1];
Block       s_block[WEEK_EVENTS];
ical::Event s_week_event[WEEK_EVENTS];
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

lv_obj_t *dotted_label(lv_obj_t *parent, std::uint32_t colour, const lv_font_t *font)
{
    lv_obj_t *label = theme::make_label(parent, "", colour, font);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    quiet(label);
    return label;
}

// Dots only shorten a label of fixed height, so a title allowed two lines is
// measured and given one or two.
std::int32_t fit_lines(lv_obj_t *label, std::int32_t width, int lines)
{
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t       size{};
    lv_text_get_size(&size, lv_label_get_text(label), font, 0, 0, width, LV_TEXT_FLAG_NONE);
    const std::int32_t height = std::min<std::int32_t>(size.y, lines * font->line_height);
    lv_obj_set_height(label, height);
    return height;
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

int whole_minutes(std::int64_t seconds)
{
    return static_cast<int>(seconds / units::kSecondsPerMinute);
}

int minutes_rounded_up(std::int64_t seconds)
{
    return static_cast<int>((seconds + units::kSecondsPerMinute - 1) / units::kSecondsPerMinute);
}

int days_from(std::int64_t now, std::int64_t at);

/** How long until `at`: minutes, then hours, and past a day the days to it,
 *  counted by the calendar, since 63 hours says less than Monday does. */
void span_of(std::int64_t now, std::int64_t at, char *out, std::size_t size)
{
    const int minutes = minutes_rounded_up(at - now);
    const int days    = days_from(now, at);
    if (minutes <= 0) {
        std::snprintf(out, size, "now");
    } else if (minutes < units::kMinutesPerHour) {
        std::snprintf(out, size, "in %d min", minutes);
    } else if (minutes < units::kMinutesPerHour * units::kHoursPerDay || days < 1) {
        std::snprintf(out, size, "in %dh %02dm", minutes / units::kMinutesPerHour,
                      minutes % units::kMinutesPerHour);
    } else if (days == 1) {
        std::snprintf(out, size, "tomorrow");
    } else {
        std::snprintf(out, size, "in %d days", days);
    }
}

// Days counted from today, by the calendar rather than by 24-hour steps.
int days_from(std::int64_t now, std::int64_t at)
{
    std::tm today = local(now);
    std::tm then  = local(at);
    today.tm_hour = then.tm_hour = NOON;
    today.tm_min = then.tm_min = today.tm_sec = then.tm_sec = 0;
    // Rounded: across a change of clocks the gap is an hour off a whole day.
    return static_cast<int>(std::lround(std::difftime(std::mktime(&then), std::mktime(&today)) /
                                        static_cast<double>(units::kSecondsPerDay)));
}

void day_name(std::int64_t now, std::int64_t at, char *out, std::size_t size)
{
    static const char *const WEEKDAYS[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                           "Thursday", "Friday", "Saturday"};
    const int away = days_from(now, at);
    if (away == 0) {
        std::snprintf(out, size, "Today");
    } else if (away == 1) {
        std::snprintf(out, size, "Tomorrow");
    } else if (away == -1) {
        std::snprintf(out, size, "Yesterday");
    } else {
        const std::tm when = local(at);
        if (std::abs(away) < units::kDaysPerWeek) {
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

bool has_place(const ical::Event &event)
{
    return place_of(event)[0] != '\0';
}

const travel::Option *s_going = nullptr;

bool is_mode(const char *mode, const char *name)
{
    return std::strcmp(mode, name) == 0;
}

bool is_walk_or_bike(const char *mode)
{
    return is_mode(mode, "walk") || is_mode(mode, "bike");
}

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

// The backend writes a trip with changes as "SPR +1".
constexpr char CHANGES_MARK[] = " +";

void mode_of(const travel::Leg &leg, char *out, std::size_t size)
{
    const bool train = is_mode(leg.mode, "train");
    const bool walk  = is_mode(leg.mode, "walk");
    const int  mins  = minutes_rounded_up(leg.arrive - leg.depart);
    // Not timed yet, only allowed for: said as about so long.
    const char *about = leg.estimated ? "about " : "";
    if (is_walk_or_bike(leg.mode)) {
        std::snprintf(out, size, "%s, %s%d min", walk ? "Walk" : "Bike", about, mins);
    } else if (train && leg.line[0] != '\0') {
        char        kind[travel::kLineMax];
        const char *plus    = std::strstr(leg.line, CHANGES_MARK);
        const int   changes = plus != nullptr ? std::atoi(plus + std::strlen(CHANGES_MARK)) : 0;
        std::snprintf(kind, sizeof(kind), "%.*s",
                      static_cast<int>(plus != nullptr ? plus - leg.line : std::strlen(leg.line)),
                      leg.line);
        if (changes > 0) {
            std::snprintf(out, size, "%s, %d min, %d change%s", line_name(kind), mins, changes,
                          changes == 1 ? "" : "s");
        } else {
            std::snprintf(out, size, "%s, %d min", line_name(kind), mins);
        }
    } else if (leg.line[0] != '\0') {
        std::snprintf(out, size, "Bus %s, %s%d min", leg.line, about, mins);
    } else {
        std::snprintf(out, size, "%s, %s%d min", train ? "Train" : "Bus", about, mins);
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

void keep_ways_worth_taking(bool wanted, std::int64_t now)
{
    static travel::Option options[travel::kOptionsMax];
    const int found = wanted ? travel::options(options, travel::kOptionsMax) : 0;

    s_way_count = 0;
    for (int i = 0; i < found; ++i) {
        if (options[i].leave >= now) {
            s_ways[s_way_count++] = options[i];
        }
    }
    if (s_way_count > 0 && s_ways[0].leave - now > SHOW_WAYS_WITHIN) {
        s_way_count = 0;
    }
}

const travel::Option *picked_way()
{
    for (int i = 0; i < s_way_count; ++i) {
        if (s_ways[i].leave == s_picked_leave) {
            return &s_ways[i];
        }
    }
    return nullptr;
}

const travel::Option *best_way()
{
    for (int i = 0; i < s_way_count; ++i) {
        if (!s_ways[i].late) {
            return &s_ways[i];
        }
    }
    return s_way_count > 0 ? &s_ways[0] : nullptr;
}

void order_ways_by_leaving()
{
    for (int i = 0; i < s_way_count; ++i) {
        int at = i;
        for (; at > 0 && s_ways[s_way_order[at - 1]].leave > s_ways[i].leave; --at) {
            s_way_order[at] = s_way_order[at - 1];
        }
        s_way_order[at] = i;
    }
}

void pick_journey(bool wanted, std::int64_t now)
{
    keep_ways_worth_taking(wanted, now);
    s_going = picked_way();
    if (s_going == nullptr) {
        s_picked_leave = 0;
        s_going        = best_way();
    }
    order_ways_by_leaving();
}

// Where the train takes you, which is what tells one way from another: the
// station a bus goes on from, or the one you walk from.
const char *via_of(const travel::Option &way)
{
    for (int i = 0; i < way.leg_count; ++i) {
        if (is_mode(way.legs[i].mode, "train")) {
            return way.legs[i].to;
        }
    }
    return way.leg_count > 0 ? way.legs[0].to : "";
}

// A tap on an event anywhere shows it whole: the strip and the week draw a
// short one as a sliver with no room for words.
std::optional<ModalOverlay> s_detail;
lv_obj_t                   *s_detail_dot   = nullptr;
lv_obj_t                   *s_detail_kind  = nullptr;
lv_obj_t                   *s_detail_title = nullptr;
lv_obj_t                   *s_detail_when  = nullptr;
lv_obj_t                   *s_detail_place = nullptr;

void when_of(const ical::Event &event, char *out, std::size_t size)
{
    char from[16];
    char to[16];
    clock_of(event.start, from, sizeof(from));
    clock_of(event.end, to, sizeof(to));
    const int mins  = whole_minutes(event.end - event.start);
    const int hours = mins / units::kMinutesPerHour;
    const int rest  = mins % units::kMinutesPerHour;
    if (rest == 0) {
        std::snprintf(out, size, "%s \xe2\x80\x93 %s, %d h", from, to, hours);
    } else if (mins > units::kMinutesPerHour) {
        std::snprintf(out, size, "%s \xe2\x80\x93 %s, %d h %d min", from, to, hours, rest);
    } else {
        std::snprintf(out, size, "%s \xe2\x80\x93 %s, %d min", from, to, mins);
    }
}

void open_detail(const ical::Event &event)
{
    if (!s_detail.has_value()) {
        return;
    }
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    char       day[24];
    char       text[96];
    day_name(now, event.start, day, sizeof(day));
    std::snprintf(text, sizeof(text), "%s, %s", feed_kind(event.feed), day);
    theme::set_text(s_detail_kind, text);
    lv_obj_set_style_bg_color(s_detail_dot, lv_color_hex(feed_ink(event.feed)), 0);
    theme::set_text(s_detail_title, event.summary);

    when_of(event, text, sizeof(text));
    theme::set_text(s_detail_when, text);
    theme::set_text(s_detail_place, place_of(event));
    lv_obj_set_hidden(s_detail_place, !has_place(event));

    lv_obj_t *body = lv_obj_get_parent(s_detail_title);
    lv_obj_update_layout(body);
    s_detail->resize(DETAIL_W, lv_obj_get_height(body) + 2 * space::l);
    s_detail->open();
}

void build_detail(lv_obj_t *page)
{
    s_detail.emplace(page, DETAIL_W, DETAIL_H_UNMEASURED);
    lv_obj_t *card = s_detail->content();
    lv_obj_t *body = column_of(card, space::s);
    lv_obj_set_pos(body, space::l, space::l);
    lv_obj_set_width(body, DETAIL_W - 2 * space::l - ModalOverlay::header_height());

    lv_obj_t *kind = row_of(body, theme::type_label()->line_height, space::s);
    s_detail_dot   = dot_of(kind);
    s_detail_kind  = line_label(kind, theme::secondary, theme::type_label());

    s_detail_title = theme::make_label(body, "", theme::text, theme::type_title());
    lv_obj_set_width(s_detail_title, LV_PCT(100));
    lv_label_set_long_mode(s_detail_title, LV_LABEL_LONG_MODE_WRAP);
    quiet(s_detail_title);

    s_detail_when  = line_label(body, theme::text, theme::type_body());
    s_detail_place = line_label(body, theme::secondary, theme::type_body());
    lv_obj_set_width(s_detail_place, LV_PCT(100));
    s_detail->add_close_button();
}

void tappable(lv_obj_t *block, const ical::Event *events, int index)
{
    lv_obj_set_clickable(block, true);
    lv_obj_set_ext_click_area(block, space::xs);
    lv_obj_add_event_cb(
        block,
        [](lv_event_t *e) {
            open_detail(*static_cast<const ical::Event *>(lv_event_get_user_data(e)));
        },
        LV_EVENT_CLICKED, const_cast<ical::Event *>(&events[index]));
}

// Without a route the event card keeps the column, its lines centred rather than
// spread to the corners.
bool         s_day_shown = false;
std::int64_t s_day_until = 0;  // the end of the day the strip shows
bool s_day_relayout = false;  // the strip needs drawing again at the card's size

struct DayBounds {
    std::int64_t from;
    std::int64_t to;
};

DayBounds day_around(std::int64_t at)
{
    std::tm day = local(at);
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    const std::int64_t from = static_cast<std::int64_t>(std::mktime(&day));
    day.tm_mday += 1;
    day.tm_isdst = -1;
    const std::int64_t to = static_cast<std::int64_t>(std::mktime(&day));
    return {from, to};
}

// The hours of one day laid across the strip's width.
struct DayScale {
    std::int64_t from;
    float        first;
    float        last;
    float        span;

    float hour_at(std::int64_t at) const
    {
        return static_cast<float>(std::clamp<std::int64_t>(at - from, 0, units::kSecondsPerDay)) /
               static_cast<float>(units::kSecondsPerHour);
    }

    std::int32_t x_of_hour(float hour) const
    {
        return static_cast<std::int32_t>((hour - first) / span * static_cast<float>(s_day_w));
    }

    std::int32_t x_of(std::int64_t at) const { return x_of_hour(hour_at(at)); }
};

DayScale scale_for(std::int64_t from, const ical::Event *events, int count)
{
    DayScale scale{from, WORKDAY_FIRST_HOUR, WORKDAY_LAST_HOUR, 0.0f};
    for (int i = 0; i < count; ++i) {
        scale.first = std::min(scale.first, std::floor(scale.hour_at(events[i].start)));
        scale.last  = std::max(scale.last, std::ceil(scale.hour_at(events[i].end)));
    }
    scale.span = std::max(1.0f, scale.last - scale.first);
    return scale;
}

std::int32_t day_ticks_h()
{
    return theme::type_label()->line_height + space::s;
}

int day_lane_count()
{
    return s_day_two ? DAY_LANES : 1;
}

void show_day_ticks(const DayScale &scale)
{
    const int step = scale.span > HOURLY_MARKS_MAX ? SPARSE_MARK_STEP : 1;
    int       tick = 0;
    char      text[40];
    for (int h = static_cast<int>(scale.first); h <= static_cast<int>(scale.last) && tick < DAY_TICKS;
         h += step) {
        std::snprintf(text, sizeof(text), "%02d", h);
        theme::set_text(s_day_tick[tick], text);
        const std::int32_t at = scale.x_of_hour(static_cast<float>(h));
        lv_obj_set_pos(s_day_tick[tick], std::clamp<std::int32_t>(at - TICK_LEAD, 0, s_day_w - TICK_DIGITS_W),
                       0);
        lv_obj_set_hidden(s_day_tick[tick++], false);
    }
    for (; tick < DAY_TICKS; ++tick) {
        lv_obj_set_hidden(s_day_tick[tick], true);
    }
}

void show_day_block(Block &block, const ical::Event &event, int lane, const DayScale &scale,
                    const ical::Event &next, std::int64_t now)
{
    const std::int32_t x = scale.x_of(event.start);
    const std::int32_t w = std::max<std::int32_t>(scale.x_of(event.end) - x - DAY_BLOCK_GAP, DAY_BLOCK_MIN_W);
    lv_obj_set_pos(block.root, x, day_ticks_h() + lane * (s_day_lane_h + space::s));
    lv_obj_set_size(block.root, w, s_day_lane_h);

    const bool coming = event.start == next.start && std::strcmp(event.summary, next.summary) == 0;
    const bool over   = event.end < now;
    const auto ink    = feed_ink(event.feed);
    lv_obj_set_style_bg_color(block.root,
                              lv_color_mix(lv_color_hex(ink), lv_color_hex(theme::panel),
                                           coming ? NEXT_BLOCK_MIX : DAY_BLOCK_MIX),
                              0);
    lv_obj_set_style_border_color(block.root, lv_color_hex(ink), 0);
    lv_obj_set_style_opa(block.root, over ? LV_OPA_50 : LV_OPA_COVER, 0);

    char text[40];
    clock_of(event.start, text, sizeof(text));
    theme::set_text(block.time, text);
    theme::set_text(block.title, event.summary);
    theme::set_text(block.place, place_of(event));
    const std::int32_t text_w = w - BLOCK_TEXT_INSET;
    lv_obj_set_width(block.time, text_w);
    lv_obj_set_width(block.title, text_w);
    lv_obj_set_width(block.place, text_w);
    // A title in a sliver breaks every word apart; the time is enough there.
    const std::int32_t line  = theme::type_label()->line_height;
    const std::int32_t room  = s_day_lane_h - 2 * DAY_BLOCK_PAD_TOP - line;
    const bool         wide  = w >= DAY_TIME_MIN_W;
    const bool         words = w >= DAY_TITLE_MIN_W;
    lv_obj_set_hidden(block.time, !wide);
    lv_obj_set_hidden(block.title, !words || room < line);
    fit_lines(block.title, text_w, std::max<std::int32_t>(1, room / line - 1));
    lv_obj_set_hidden(block.place, !words || !has_place(event) || room < 2 * line);
    lv_obj_set_hidden(block.root, false);
}

void show_day(const ical::Event *next, std::int64_t now)
{
    s_day_shown = false;
    if (next == nullptr) {
        return;
    }
    const DayBounds day = day_around(next->start);

    static ical::Event events[DAY_BLOCKS];
    const int count = ical::between(day.from, day.to, events, DAY_BLOCKS);
    if (count == 0) {
        return;
    }
    s_day_shown = true;
    s_day_until = day.to;

    const DayScale scale = scale_for(day.from, events, count);
    show_day_ticks(scale);

    std::int64_t lane_end[DAY_LANES] = {0, 0};
    for (int i = 0; i < DAY_BLOCKS; ++i) {
        if (i >= count) {
            lv_obj_set_hidden(s_day_block[i].root, true);
            continue;
        }
        const ical::Event &event = events[i];
        s_day_event[i]           = event;
        const int lane = event.start >= lane_end[0] ? 0 : 1;
        lane_end[lane] = std::max(lane_end[lane], event.end);
        show_day_block(s_day_block[i], event, lane, scale, *next, now);
    }
    s_day_two = lane_end[1] != 0;
    const int          lanes   = day_lane_count();
    const std::int32_t lanes_y = day_ticks_h();
    const std::int32_t track_h = lanes_y + lanes * s_day_lane_h + (lanes - 1) * space::s;
    lv_obj_set_height(s_day_track, track_h);

    const bool today = now >= day.from && now < day.to && scale.hour_at(now) >= scale.first &&
                       scale.hour_at(now) <= scale.last;
    lv_obj_set_hidden(s_day_now, !today);
    if (today) {
        lv_obj_set_pos(s_day_now, scale.x_of(now) - NOW_LINE_W / 2, lanes_y);
        lv_obj_set_height(s_day_now, track_h - lanes_y);
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
    const int          lanes = day_lane_count();
    s_day_lane_h = std::clamp<std::int32_t>((inner - day_ticks_h() - (lanes - 1) * space::s) / lanes,
                                            DAY_LANE_MIN_H, DAY_LANE_MAX_H);
    s_day_relayout = true;
}

void show_choice(const Choice &choice, const travel::Option &way, std::int64_t starts)
{
    const bool on = &way == s_going;
    lv_obj_set_state(choice.root, LV_STATE_CHECKED, on);
    char text[16];
    clock_of(way.leave, text, sizeof(text));
    theme::set_text(choice.time, text);
    theme::set_text_color(choice.time, way.cancelled ? theme::red : theme::text);
    if (way.late && starts > 0) {
        std::snprintf(text, sizeof(text), "%d min late", minutes_rounded_up(way.arrive - starts));
        theme::set_text(choice.via, text);
    } else {
        theme::set_text(choice.via, via_of(way));
    }
    theme::set_text_color(choice.via, on          ? theme::text
                                      : way.late ? theme::amber
                                                 : theme::secondary);
}

// The ways there, side by side, when there is more than one to choose from.
void show_choices(std::int64_t starts)
{
    const bool pick = s_way_count > 1;
    lv_obj_set_hidden(s_choices, !pick);
    lv_obj_set_hidden(s_route_name, pick);
    for (int i = 0; i < travel::kOptionsMax; ++i) {
        const bool real = pick && i < s_way_count;
        lv_obj_set_hidden(s_choice[i].root, !real);
        if (real) {
            show_choice(s_choice[i], s_ways[s_way_order[i]], starts);
        }
    }

    // The route takes what is left of the card, clear of the corner chip.
    const std::int32_t head = pick ? CHOICE_H + space::l
                                   : theme::type_label()->line_height + space::l;
    lv_obj_set_y(s_route, head);
    s_route_h = s_page_h - 2 * space::l - head - CHIP_CLEARANCE;
    lv_obj_set_height(s_route, s_route_h);
}

struct RouteLayout {
    std::int32_t pitch;
    std::int32_t line;
    std::int32_t rail_x;
    std::int32_t text_x;
};

// Returns whether the note says the way arrives late.
bool note_of(int stop, std::int64_t starts, char *out, std::size_t size)
{
    const int  legs   = s_going->leg_count;
    const bool leaves = stop < legs;
    // Walking or riding onto a train is not a change, nor getting off to walk on.
    if (stop > 0 && leaves && !is_walk_or_bike(s_going->legs[stop - 1].mode) &&
        !is_walk_or_bike(s_going->legs[stop].mode)) {
        const int wait = whole_minutes(s_going->legs[stop].depart - s_going->legs[stop - 1].arrive);
        std::snprintf(out, size, "change, %d min", wait);
        return false;
    }
    if (!leaves && starts > 0) {
        // Counted from being there, the walk at the far end and all, as the
        // choices count late: from the last stop it said early and late at once.
        const int spare = whole_minutes(starts - s_going->arrive);
        if (spare >= 0) {
            std::snprintf(out, size, "%d min before it starts", spare);
        } else {
            std::snprintf(out, size, "%d min late", -spare);
        }
        return spare < 0;
    }
    out[0] = '\0';
    return false;
}

// Where the walk at the end goes, which the backend leaves to be named: the
// room the event is in, or just there.
const char *end_name(const char *place)
{
    return place != nullptr && place[0] != '\0' ? place : "There";
}

void show_stop(int i, const RouteLayout &layout, std::int64_t starts, const char *place)
{
    const Stop &stop = s_stop[i];
    const int   legs = s_going->leg_count;
    const bool  real = i <= legs;
    lv_obj_set_hidden(stop.node, !real);
    lv_obj_set_hidden(stop.when, !real);
    lv_obj_set_hidden(stop.name, !real);
    lv_obj_set_hidden(stop.note, !real);
    if (!real) {
        return;
    }
    const std::int32_t y         = i * layout.pitch;
    const bool         leaves    = i < legs;
    const bool         ends      = i == 0 || i == legs;
    const bool         cancelled = leaves && s_going->legs[i].cancelled;

    lv_obj_set_pos(stop.node, layout.rail_x - NODE / 2, y + (layout.line - NODE) / 2);
    lv_obj_set_style_bg_color(stop.node, lv_color_hex(ends ? theme::secondary : theme::panel_light),
                              0);

    char text[64];
    clock_of(leaves ? s_going->legs[i].depart : s_going->legs[legs - 1].arrive, text,
             sizeof(text));
    theme::set_text(stop.when, text);
    // A time only allowed for, not known, is said more quietly.
    const bool guessed = leaves ? s_going->legs[i].estimated : s_going->legs[legs - 1].estimated;
    theme::set_text_color(stop.when, cancelled ? theme::red : guessed ? theme::secondary : theme::text);
    lv_obj_set_pos(stop.when, 0, y);

    const char *last = s_going->legs[legs - 1].to;
    theme::set_text(stop.name, leaves ? s_going->legs[i].from : last[0] != '\0' ? last : end_name(place));
    lv_obj_set_pos(stop.name, layout.text_x, y);

    const bool late = note_of(i, starts, text, sizeof(text));
    theme::set_text(stop.note, text);
    theme::set_text_color(stop.note, late ? theme::amber : theme::secondary);
    lv_obj_set_pos(stop.note, layout.text_x, y + layout.line);
}

const lv_image_dsc_t *icon_of(const char *mode)
{
    if (is_mode(mode, "walk")) {
        return &icons::walk_icon;
    }
    if (is_mode(mode, "bike")) {
        return &icons::bike_icon;
    }
    return is_mode(mode, "train") ? &icons::train_icon : &icons::bus_icon;
}

void show_ride(int i, const RouteLayout &layout)
{
    const Ride &ride = s_ride[i];
    const bool  real = i < s_going->leg_count;
    lv_obj_set_hidden(ride.rail, !real);
    lv_obj_set_hidden(ride.icon, !real);
    lv_obj_set_hidden(ride.what, !real);
    if (!real) {
        return;
    }
    const travel::Leg &leg  = s_going->legs[i];
    const std::int32_t top  = i * layout.pitch + layout.line / 2;
    const bool         path = is_walk_or_bike(leg.mode);
    const std::int32_t rail = path ? PATH_W : RAIL_W;
    lv_obj_set_pos(ride.rail, layout.rail_x - rail / 2, top);
    lv_obj_set_size(ride.rail, rail, layout.pitch);
    lv_obj_set_style_bg_color(ride.rail, lv_color_hex(leg.cancelled ? theme::red : theme::panel),
                              0);

    const std::int32_t label_h = theme::type_label()->line_height;
    const bool         noted   = lv_label_get_text(s_stop[i].note)[0] != '\0';
    const std::int32_t mid     = top + ((noted ? label_h : 0) + layout.pitch) / 2;
    lv_image_set_src(ride.icon, icon_of(leg.mode));
    lv_obj_set_style_image_recolor(ride.icon,
                                   lv_color_hex(leg.cancelled ? theme::red : theme::secondary), 0);
    lv_obj_set_pos(ride.icon, layout.text_x, mid - icons::bus_icon.header.h / 2);

    char text[64];
    mode_of(leg, text, sizeof(text));
    if (leg.cancelled) {
        std::strncat(text, ", cancelled", sizeof(text) - std::strlen(text) - 1);
    }
    theme::set_text(ride.what, text);
    theme::set_text_color(ride.what, leg.cancelled ? theme::red : theme::secondary);
    lv_obj_set_pos(ride.what,
                   layout.text_x + static_cast<std::int32_t>(icons::bus_icon.header.w) + space::s,
                   mid - label_h / 2);
}

void show_journey(std::int64_t starts, const char *place)
{
    if (s_going == nullptr) {
        return;
    }
    show_choices(starts);

    RouteLayout layout{};
    layout.line   = theme::type_body()->line_height;
    layout.pitch  = std::min<std::int32_t>(
        STOP_PITCH_MAX,
        (s_route_h - layout.line - theme::type_label()->line_height) / std::max(s_going->leg_count, 1));
    layout.rail_x = STOP_TIME_W + space::s + NODE / 2;
    layout.text_x = layout.rail_x + NODE / 2 + space::m;

    for (int i = 0; i < POINTS; ++i) {
        show_stop(i, layout, starts, place);
    }
    for (int i = 0; i < travel::kLegsMax; ++i) {
        show_ride(i, layout);
    }
}

void show_nothing_next()
{
    lv_obj_set_hidden(s_kind_dot, true);
    theme::set_text(s_kind, "");
    theme::set_text(s_title, "Nothing coming up");
    theme::set_text(s_meta, "");
    theme::set_text(s_big_name, "");
    theme::set_text(s_big, "");
    theme::set_text(s_big_note, "");
    theme::set_text(s_side, "");
}

void show_big_number(const char *name, const char *value, std::uint32_t colour, const char *note)
{
    theme::set_text(s_big_name, name);
    theme::set_text(s_big, value);
    theme::set_text_color(s_big, colour);
    theme::set_text(s_big_note, note);
    theme::set_text(s_side, "");
}

void show_next(const ical::Event *first, std::int64_t now)
{
    if (first == nullptr) {
        show_nothing_next();
        return;
    }

    char       text[96];
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
    fit_lines(s_title, s_title_w, TITLE_LINES_MAX);

    char from[16];
    char to[16];
    clock_of(first->start, from, sizeof(from));
    clock_of(first->end, to, sizeof(to));
    if (has_place(*first)) {
        std::snprintf(text, sizeof(text), "%s \xe2\x80\x93 %s, %s", from, to,
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
        span_of(now, s_going->leave, span, sizeof(span));
        show_big_number(s_going->cancelled ? "Leave, cancelled" : "Leave", text,
                        s_going->cancelled ? theme::red : theme::text, span);
    } else if (ongoing) {
        span_of(now, first->end, span, sizeof(span));
        show_big_number("Ends", to, theme::text, span);
    } else {
        span_of(now, first->start, span, sizeof(span));
        show_big_number("Starts", from, theme::text, span);
    }
}

void show_day_heading(Item &head, const char *name, bool first)
{
    lv_obj_set_hidden(head.dot, true);
    lv_obj_set_hidden(head.time, true);
    theme::set_text(head.title, name);
    theme::set_text_color(head.title, theme::secondary);
    lv_obj_set_style_text_font(head.title, theme::type_label(), 0);
    lv_obj_set_height(head.title, theme::type_label()->line_height);
    lv_obj_set_hidden(head.place, true);
    lv_obj_set_style_margin_top(head.root, first ? 0 : space::s, 0);
    lv_obj_set_hidden(head.root, false);
}

void show_list_event(Item &item, const ical::Event &event)
{
    char clock[16];
    clock_of(event.start, clock, sizeof(clock));
    lv_obj_set_hidden(item.dot, false);
    lv_obj_set_style_bg_color(item.dot, lv_color_hex(feed_ink(event.feed)), 0);
    lv_obj_set_hidden(item.time, false);
    theme::set_text(item.time, clock);
    theme::set_text(item.title, event.summary);
    theme::set_text_color(item.title, theme::text);
    lv_obj_set_style_text_font(item.title, theme::type_body(), 0);
    fit_lines(item.title, s_item_w, TITLE_LINES_MAX);
    theme::set_text(item.place, place_of(event));
    lv_obj_set_hidden(item.place, !has_place(event));
    lv_obj_set_style_margin_top(item.root, 0, 0);
    lv_obj_set_hidden(item.root, false);
}

void show_nothing_listed(Item &none)
{
    lv_obj_set_hidden(none.dot, true);
    lv_obj_set_hidden(none.time, true);
    theme::set_text(none.title, "Nothing else in the coming week");
    theme::set_text_color(none.title, theme::secondary);
    lv_obj_set_style_text_font(none.title, theme::type_body(), 0);
    lv_obj_set_height(none.title, theme::type_body()->line_height);
    lv_obj_set_hidden(none.place, true);
    lv_obj_set_hidden(none.root, false);
}

void show_list(const ical::Event *ahead, int count, std::int64_t now)
{
    // The coming seven days rather than what is left of the calendar week,
    // which on a Friday is nothing.
    const std::int64_t week_end = now + SECONDS_PER_WEEK;
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
            char name[24];
            day_name(now, ahead[i].start, name, sizeof(name));
            const bool first = used == 0;
            show_day_heading(s_item[used++], name, first);
            last_day = day;
        }
        show_list_event(s_item[used++], ahead[i]);
    }
    if (used == 0) {
        show_nothing_listed(s_item[used++]);
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
    day.tm_mday -= (day.tm_wday - MONDAY + units::kDaysPerWeek) % units::kDaysPerWeek;
    if (day.tm_wday == SATURDAY || day.tm_wday == SUNDAY) {
        day.tm_mday += units::kDaysPerWeek;
    }
    day.tm_mday += units::kDaysPerWeek * weeks;
    day.tm_hour = day.tm_min = day.tm_sec = 0;
    day.tm_isdst = -1;
    return static_cast<std::int64_t>(std::mktime(&day));
}

std::tm noon_on(std::int64_t monday, int day)
{
    return local(monday + day * units::kSecondsPerDay + NOON * units::kSecondsPerHour);
}

float hour_of(std::int64_t at)
{
    const std::tm when = local(at);
    return static_cast<float>(when.tm_hour) +
           static_cast<float>(when.tm_min) / static_cast<float>(units::kMinutesPerHour);
}

struct WeekGrid {
    std::int64_t from;
    int          days;
    float        first;
    float        last;
    int          hours;
    float        px_per_hour;
    std::int32_t col_w;
    int          today;  // off the grid on another week
};

WeekGrid grid_for(std::int64_t from, std::int64_t now, const ical::Event *week, int count,
                  int *day_of)
{
    WeekGrid grid{};
    grid.from  = from;
    grid.days  = WORKDAYS;  // the weekend only when something is on it
    grid.first = WORKDAY_FIRST_HOUR;
    grid.last  = WORKDAY_LAST_HOUR;
    for (int i = 0; i < count; ++i) {
        day_of[i]  = std::clamp(days_from(from, week[i].start), 0, WEEK_DAYS - 1);
        grid.days  = std::max(grid.days, day_of[i] + 1);
        grid.first = std::min(grid.first, std::floor(hour_of(week[i].start)));
        const bool same_day = days_from(week[i].start, week[i].end) == 0;
        grid.last = std::max(grid.last, same_day ? std::ceil(hour_of(week[i].end))
                                                 : static_cast<float>(units::kHoursPerDay));
    }
    grid.hours       = static_cast<int>(grid.last - grid.first);
    grid.px_per_hour = static_cast<float>(s_grid_h) / static_cast<float>(grid.hours);
    grid.col_w       = (s_grid_w - HOUR_AXIS_W) / grid.days;
    grid.today       = days_from(from, now);
    return grid;
}

std::int32_t column_x(const WeekGrid &grid, int day)
{
    return HOUR_AXIS_W + day * grid.col_w;
}

void show_hour_lines(const WeekGrid &grid)
{
    char               text[64];
    const std::int32_t line  = theme::type_label()->line_height;
    // A line an hour is a grid, not a week.
    const int          every = grid.hours > HOURLY_MARKS_MAX ? SPARSE_MARK_STEP : 1;
    for (int h = 0; h <= HOURS_MAX; ++h) {
        const bool real = h <= grid.hours && h % every == 0;
        lv_obj_set_hidden(s_hour_mark[h], !real || h == grid.hours);
        lv_obj_set_hidden(s_hour_line[h], !real);
        if (!real) {
            continue;
        }
        const auto y = static_cast<std::int32_t>(static_cast<float>(h) * grid.px_per_hour);
        std::snprintf(text, sizeof(text), "%02d", static_cast<int>(grid.first) + h);
        theme::set_text(s_hour_mark[h], text);
        lv_obj_set_pos(s_hour_mark[h], 0, std::max<std::int32_t>(0, y - line / 2));
        lv_obj_set_pos(s_hour_line[h], HOUR_AXIS_W, y);
        lv_obj_set_width(s_hour_line[h], s_grid_w - HOUR_AXIS_W);
    }
}

void show_today_band(const WeekGrid &grid)
{
    for (int d = 0; d < WEEK_DAYS; ++d) {
        const bool today = d == grid.today && d < grid.days;
        lv_obj_set_hidden(s_day_lane[d], !today);
        if (today) {
            lv_obj_set_pos(s_day_lane[d], column_x(grid, d) + TODAY_INSET, -space::xs);
            lv_obj_set_size(s_day_lane[d], grid.col_w - 2 * TODAY_INSET, s_grid_h + 2 * space::xs);
        }
    }
}

void show_day_heads(const WeekGrid &grid)
{
    static const char *const NAMES[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    char text[64];
    for (int d = 0; d < WEEK_DAYS; ++d) {
        lv_obj_set_hidden(s_day_head[d], d >= grid.days);
        if (d >= grid.days) {
            continue;
        }
        const bool    today = d == grid.today;
        const std::tm date  = noon_on(grid.from, d);
        std::snprintf(text, sizeof(text), "%s %d", NAMES[d], date.tm_mday);
        theme::set_text(s_day_head[d], text);
        theme::set_text_color(s_day_head[d], today ? theme::text : theme::secondary);
        lv_obj_set_style_bg_opa(s_day_head[d], today ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        const std::int32_t head_w = std::min<std::int32_t>(grid.col_w - 2 * space::s, DAY_HEAD_MAX_W);
        lv_obj_set_width(s_day_head[d], head_w);
        lv_obj_set_pos(s_day_head[d], column_x(grid, d) + (grid.col_w - head_w) / 2, 0);
    }
}

// Each event takes the first lane free at its start; a run of overlaps is as
// narrow as its widest point.
void assign_lanes(const ical::Event *week, int count, const int *day_of, int days, int *lane,
                  int *lanes_across)
{
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
            lane_end[l]         = week[i].end;
            lane[i]             = l;
            run_of[i]           = runs - 1;
            run_lanes[runs - 1] = std::max(run_lanes[runs - 1], l + 1);
            run_end             = std::max(run_end, week[i].end);
        }
    }
    for (int i = 0; i < count; ++i) {
        lanes_across[i] = run_lanes[run_of[i]];
    }
}

// The start, and the title where there is width for words: a sliver shows only
// its colour, and a tap shows the rest.
void show_week_block_words(const Block &block, const ical::Event &event, std::int32_t block_w,
                           std::int32_t h, bool over)
{
    const std::int32_t line  = theme::type_label()->line_height;
    const std::int32_t inner = block_w - BLOCK_TEXT_INSET;
    const bool         timed = inner >= WEEK_TIME_MIN_W;
    const bool         words = inner >= WEEK_TITLE_MIN_W && h >= 2 * line + WEEK_BLOCK_PAD_TOP;
    char               text[64];
    clock_of(event.start, text, sizeof(text));
    theme::set_text(block.time, text);
    theme::set_text_color(block.time, over ? theme::secondary : theme::text);
    lv_obj_set_hidden(block.time, !timed);
    theme::set_text(block.title, event.summary);
    theme::set_text_color(block.title, over ? theme::secondary : theme::text);

    // The room under the title, when a line is left over for it.
    const char        *where  = place_of(event);
    const std::int32_t spare  = std::max<std::int32_t>(1, (h - 2 * WEEK_BLOCK_PAD_TOP - line) / line);
    const bool         placed = words && where[0] != '\0' && spare >= TITLE_AND_PLACE_LINES;
    const std::int32_t lines  = placed ? std::min<std::int32_t>(spare - 1, TITLE_LINES_MAX) : spare;
    lv_obj_set_pos(block.title, 0, line);
    lv_obj_set_size(block.title, inner, lines * line);
    lv_obj_set_hidden(block.title, !words);
    const std::int32_t title_h = fit_lines(block.title, inner, lines);
    theme::set_text(block.place, where);
    lv_obj_set_width(block.place, inner);
    lv_obj_set_pos(block.place, 0, line + title_h);
    lv_obj_set_hidden(block.place, !placed);
}

void show_week_block(const Block &block, const ical::Event &event, const WeekGrid &grid, int day,
                     int lane, int lanes, std::int64_t now)
{
    const std::int32_t line   = theme::type_label()->line_height;
    const std::int32_t lane_w = (grid.col_w - 2 * WEEK_BLOCK_INSET + WEEK_BLOCK_APART) / lanes;
    const float        top_h  = std::max(hour_of(event.start), grid.first) - grid.first;
    const float        end_h  = days_from(event.start, event.end) == 0 ? hour_of(event.end) : grid.last;
    const auto         y      = static_cast<std::int32_t>(top_h * grid.px_per_hour);
    const std::int32_t h      = std::max<std::int32_t>(
        static_cast<std::int32_t>((end_h - grid.first) * grid.px_per_hour) - y - 2 * WEEK_BLOCK_CLEARANCE,
        line + WEEK_BLOCK_MIN_SPARE);
    const std::int32_t block_w = lane_w - WEEK_BLOCK_APART;
    lv_obj_set_pos(block.root, column_x(grid, day) + WEEK_BLOCK_INSET + lane * lane_w,
                   y + WEEK_BLOCK_CLEARANCE);
    lv_obj_set_size(block.root, block_w, h);

    // Faded by colour rather than by opacity, which would show the lines
    // behind through a past event.
    const std::uint32_t ink  = feed_ink(event.feed);
    const bool          over = event.end < now;
    const lv_color_t    card = lv_color_hex(theme::panel_light);
    lv_obj_set_style_bg_color(block.root,
                              lv_color_mix(lv_color_hex(ink), card, over ? PAST_BLOCK_MIX : WEEK_BLOCK_MIX),
                              0);
    lv_obj_set_style_border_color(
        block.root, over ? lv_color_mix(lv_color_hex(ink), card, WEEK_BLOCK_MIX) : lv_color_hex(ink), 0);

    show_week_block_words(block, event, block_w, h, over);
    lv_obj_set_hidden(block.root, false);
}

void show_now_line(const WeekGrid &grid, std::int64_t now)
{
    const float hour  = hour_of(now);
    const bool  shown = grid.today >= 0 && grid.today < grid.days && hour >= grid.first && hour <= grid.last;
    lv_obj_set_hidden(s_week_now, !shown);
    if (shown) {
        lv_obj_set_pos(s_week_now, column_x(grid, grid.today) + TODAY_INSET,
                       static_cast<std::int32_t>((hour - grid.first) * grid.px_per_hour) - NOW_LINE_W / 2);
        lv_obj_set_width(s_week_now, grid.col_w - 2 * TODAY_INSET);
    }
}

void show_week_name(const WeekGrid &grid)
{
    const std::tm monday = noon_on(grid.from, 0);
    const std::tm sunday = noon_on(grid.from, grid.days - 1);
    char          week_no[8];
    std::strftime(week_no, sizeof(week_no), "%V", &monday);
    char first_month[8];
    char last_month[8];
    std::strftime(first_month, sizeof(first_month), "%b", &monday);
    std::strftime(last_month, sizeof(last_month), "%b", &sunday);

    char text[64];
    std::snprintf(text, sizeof(text), "W%s", week_no);
    theme::set_text(s_week_no, text);
    if (monday.tm_mon == sunday.tm_mon) {
        std::snprintf(text, sizeof(text), "%d \xe2\x80\x93 %d %s", monday.tm_mday, sunday.tm_mday,
                      last_month);
    } else {
        std::snprintf(text, sizeof(text), "%d %s \xe2\x80\x93 %d %s", monday.tm_mday, first_month,
                      sunday.tm_mday, last_month);
    }
    theme::set_text(s_week_name, text);
}

void show_week(std::int64_t now)
{
    static ical::Event week[WEEK_EVENTS];
    const std::int64_t from = week_start(now, s_week_shift);
    // An hour over, for the week summer time ends.
    const int count = ical::between(from, from + SECONDS_PER_WEEK + units::kSecondsPerHour, week, WEEK_EVENTS);

    int            day_of[WEEK_EVENTS];
    const WeekGrid grid = grid_for(from, now, week, count, day_of);
    show_hour_lines(grid);
    show_today_band(grid);
    show_day_heads(grid);

    int lane[WEEK_EVENTS];
    int lanes_across[WEEK_EVENTS];
    assign_lanes(week, count, day_of, grid.days, lane, lanes_across);
    for (int i = 0; i < WEEK_EVENTS; ++i) {
        if (i >= count) {
            lv_obj_set_hidden(s_block[i].root, true);
            continue;
        }
        s_week_event[i] = week[i];
        show_week_block(s_block[i], week[i], grid, day_of[i], lane[i], lanes_across[i], now);
    }

    show_now_line(grid, now);
    show_week_name(grid);
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

void build_next_card(std::int32_t left_w)
{
    s_next = theme::make_card(s_overview);
    lv_obj_set_pos(s_next, 0, 0);
    lv_obj_set_size(s_next, left_w, s_page_h);
    s_title_w = left_w - 2 * space::l;
    lv_obj_set_flex_flow(s_next, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_next, space::xs, 0);
    quiet(s_next);

    lv_obj_t *kind = row_of(s_next, theme::type_label()->line_height, space::s);
    s_kind_dot     = dot_of(kind);
    s_kind         = line_label(kind, theme::secondary, theme::type_label());

    s_title = dotted_label(s_next, theme::text, theme::type_title());
    lv_obj_set_width(s_title, LV_PCT(100));

    s_meta = line_label(s_next, theme::secondary, theme::type_body());
    lv_obj_set_width(s_meta, LV_PCT(100));

    lv_obj_t *spread = bare(s_next);
    lv_obj_set_size(spread, 1, space::l);

    lv_obj_t *numbers = row_of(s_next, LV_SIZE_CONTENT, space::xl);
    lv_obj_set_flex_align(numbers, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *big = column_of(numbers, 0);
    lv_obj_set_width(big, LV_SIZE_CONTENT);
    s_big_name = line_label(big, theme::secondary, theme::type_label());
    s_big      = line_label(big, theme::text, theme::type_display());
    lv_obj_t *side = column_of(numbers, 0);
    lv_obj_set_width(side, LV_SIZE_CONTENT);
    s_big_note = line_label(side, theme::text, theme::type_value());
    s_side     = line_label(side, theme::secondary, theme::type_body());
    lv_obj_set_style_margin_bottom(side, BASELINE_LIFT, 0);
}

void choice_clicked(lv_event_t *e)
{
    const auto index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (index < s_way_count) {
        s_picked_leave = s_ways[s_way_order[index]].leave;
        show_calendar();
    }
}

void build_choices(std::int32_t route_w)
{
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
        lv_obj_add_event_cb(choice.root, choice_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
    }
}

void build_journey_card(std::int32_t x, std::int32_t route_w)
{
    s_journey = theme::make_card(s_overview);
    lv_obj_set_pos(s_journey, x, 0);
    lv_obj_set_size(s_journey, LIST_W, s_page_h);
    lv_obj_set_hidden(s_journey, true);
    quiet(s_journey);

    s_route_name = theme::make_eyebrow(s_journey, "THE WAY THERE");
    build_choices(route_w);

    s_route = bare(s_journey);
    lv_obj_set_size(s_route, route_w, s_page_h - 2 * space::l);
}

lv_obj_t *event_block(lv_obj_t *parent, std::int32_t radius, std::int32_t pad_top)
{
    lv_obj_t *block = bare(parent);
    lv_obj_set_style_bg_opa(block, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(block, radius, 0);
    lv_obj_set_style_border_side(block, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(block, BLOCK_EDGE_W, 0);
    lv_obj_set_style_pad_left(block, BLOCK_PAD_LEFT, 0);
    lv_obj_set_style_pad_top(block, pad_top, 0);
    return block;
}

void build_day_strip(std::int32_t left_w)
{
    s_day = theme::make_card(s_overview);
    lv_obj_set_pos(s_day, 0, EVENT_H + space::m);
    lv_obj_set_size(s_day, left_w, s_page_h - EVENT_H - space::m);
    lv_obj_set_hidden(s_day, true);
    quiet(s_day);
    s_day_w     = left_w - 2 * space::l;
    s_day_track = bare(s_day);
    lv_obj_set_size(s_day_track, s_day_w, DAY_LANES * DAY_LANE_MIN_H);
    for (lv_obj_t *&tick : s_day_tick) {
        tick = line_label(s_day_track, theme::secondary, theme::type_label());
        lv_obj_set_width(tick, TICK_W);
    }
    for (Block &block : s_day_block) {
        block.root = event_block(s_day_track, theme::radius::row, DAY_BLOCK_PAD_TOP);
        lv_obj_set_flex_flow(block.root, LV_FLEX_FLOW_COLUMN);
        block.time  = line_label(block.root, theme::text, theme::type_label());
        block.title = dotted_label(block.root, theme::text, theme::type_label());
        block.place = line_label(block.root, theme::secondary, theme::type_label());
        lv_obj_set_hidden(block.root, true);
        tappable(block.root, s_day_event, static_cast<int>(&block - s_day_block));
    }
    s_day_now = bare(s_day_track);
    lv_obj_set_width(s_day_now, NOW_LINE_W);
    lv_obj_set_style_bg_opa(s_day_now, LV_OPA_COVER, 0);
    theme::fill_accent(s_day_now);
}

void build_route(std::int32_t route_w)
{
    for (Ride &ride : s_ride) {  // first, so the stops sit on top of the line
        ride.rail = bare(s_route);
        lv_obj_set_size(ride.rail, RAIL_W, 0);
        lv_obj_set_style_bg_opa(ride.rail, LV_OPA_COVER, 0);
        ride.icon = lv_image_create(s_route);
        lv_obj_set_style_image_recolor_opa(ride.icon, LV_OPA_COVER, 0);
        quiet(ride.icon);
        ride.what = line_label(s_route, theme::secondary, theme::type_label());
        lv_obj_set_width(ride.what, route_w - STOP_TIME_W - RIDE_TEXT_TRIM);
    }
    for (Stop &stop : s_stop) {
        stop.node = bare(s_route);
        lv_obj_set_size(stop.node, NODE, NODE);
        lv_obj_set_style_radius(stop.node, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(stop.node, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(stop.node, NODE_BORDER_W, 0);
        lv_obj_set_style_border_color(stop.node, lv_color_hex(theme::secondary), 0);
        stop.when = line_label(s_route, theme::text, theme::type_body());
        lv_obj_set_width(stop.when, STOP_TIME_W);
        stop.name = line_label(s_route, theme::text, theme::type_body());
        lv_obj_set_width(stop.name, route_w - STOP_TIME_W - STOP_TEXT_TRIM);
        stop.note = line_label(s_route, theme::secondary, theme::type_label());
        lv_obj_set_width(stop.note, route_w - STOP_TIME_W - STOP_TEXT_TRIM);
    }
}

void build_list_item(Item &item)
{
    const std::int32_t line = theme::type_body()->line_height;
    item.root = row_of(s_list, LV_SIZE_CONTENT, space::s);
    lv_obj_set_flex_align(item.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    item.dot = dot_of(item.root);
    lv_obj_set_style_margin_top(item.dot, (line - DOT) / 2, 0);
    item.time = line_label(item.root, theme::secondary, theme::type_body());
    lv_obj_set_width(item.time, TIME_W);
    lv_obj_t *what = column_of(item.root, 0);
    lv_obj_set_width(what, 0);
    lv_obj_set_flex_grow(what, 1);
    item.title = dotted_label(what, theme::text, theme::type_body());
    lv_obj_set_width(item.title, LV_PCT(100));
    item.place = line_label(what, theme::secondary, theme::type_label());
    lv_obj_set_width(item.place, LV_PCT(100));
    lv_obj_set_hidden(item.root, true);
}

void build_coming_up(std::int32_t x)
{
    s_after = theme::make_card(s_overview);
    lv_obj_set_pos(s_after, x, 0);
    lv_obj_set_size(s_after, LIST_W, s_page_h);
    quiet(s_after);

    theme::make_eyebrow(s_after, "COMING UP");
    const std::int32_t list_y = theme::type_label()->line_height + space::l;
    s_list = column_of(s_after, space::s);
    lv_obj_set_y(s_list, list_y);
    lv_obj_set_height(s_list, s_page_h - 2 * space::l - list_y - theme::chip::size);
    lv_obj_set_clickable(s_list, true);
    lv_obj_set_scrollable(s_list, true);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(theme::secondary), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_list, SCROLLBAR_W, LV_PART_SCROLLBAR);
    for (Item &item : s_item) {
        build_list_item(item);
    }
    s_item_w = LIST_W - 2 * space::l - DOT - TIME_W - 2 * space::s;

    // The list scrolls on below; its last lines fade into the card, rather than
    // stop at a day's name whose events are out of sight.
    lv_obj_t *fade = lv_obj_create(s_after);
    lv_obj_set_size(fade, LIST_W - 2 * space::l, LIST_FADE_H);
    lv_obj_set_pos(fade, 0, list_y + lv_obj_get_style_height(s_list, LV_PART_MAIN) - LIST_FADE_H);
    theme::style_panel(fade, theme::panel_light, 0);
    lv_obj_set_style_bg_main_opa(fade, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_hex(theme::panel_light), 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_clickable(fade, false);
}

void build_overview(lv_obj_t *parent, std::int32_t width, std::int32_t height)
{
    s_overview = bare(parent);
    lv_obj_set_size(s_overview, width, height);
    s_page_h = height;

    const std::int32_t left_w  = width - LIST_W - space::m;
    const std::int32_t right_x = width - LIST_W;
    const std::int32_t route_w = LIST_W - 2 * space::l;
    build_next_card(left_w);
    build_journey_card(right_x, route_w);
    build_day_strip(left_w);
    build_route(route_w);
    build_coming_up(right_x);
}

void build_week_heads(std::int32_t day_head_h)
{
    s_week_no = line_label(s_agenda, theme::text, theme::type_label());
    lv_obj_set_width(s_week_no, WEEK_NO_W);
    lv_obj_set_pos(s_week_no, 0, DAY_HEAD_PAD_V);
    for (lv_obj_t *&head : s_day_head) {
        head = line_label(s_agenda, theme::secondary, theme::type_label());
        lv_obj_set_height(head, day_head_h);
        lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_radius(head, theme::radius::pill, 0);
        lv_obj_set_style_pad_ver(head, DAY_HEAD_PAD_V, 0);
        theme::fill_accent(head);
    }
}

void build_week_grid(std::int32_t top)
{
    lv_obj_t *grid = bare(s_agenda);
    lv_obj_set_pos(grid, 0, top);
    lv_obj_set_overflow_visible(grid, true);  // today's band reaches past it
    lv_obj_set_size(grid, s_grid_w, s_grid_h);

    // Only today's column has a ground, so the week reads as its events rather
    // than as a grid of dark columns.
    for (lv_obj_t *&lane : s_day_lane) {
        lane = bare(grid);
        lv_obj_set_style_bg_color(lane, lv_color_hex(theme::panel), 0);
        lv_obj_set_style_bg_opa(lane, LV_OPA_60, 0);
        lv_obj_set_style_radius(lane, theme::radius::row, 0);
    }
    for (int h = 0; h <= HOURS_MAX; ++h) {
        s_hour_line[h] = bare(grid);
        lv_obj_set_height(s_hour_line[h], 1);
        lv_obj_set_style_bg_color(s_hour_line[h], lv_color_hex(theme::panel), 0);
        lv_obj_set_style_bg_opa(s_hour_line[h], LV_OPA_COVER, 0);
        s_hour_mark[h] = line_label(grid, theme::secondary, theme::type_label());
        lv_obj_set_style_text_opa(s_hour_mark[h], LV_OPA_70, 0);
    }

    for (Block &block : s_block) {
        block.root = event_block(grid, theme::radius::row / 2, WEEK_BLOCK_PAD_TOP);
        lv_obj_set_hidden(block.root, true);
        block.time = line_label(block.root, theme::text, theme::type_label());
        lv_obj_set_style_text_opa(block.time, LV_OPA_80, 0);
        block.title = dotted_label(block.root, theme::text, theme::type_label());
        block.place = line_label(block.root, theme::secondary, theme::type_label());
        tappable(block.root, s_week_event, static_cast<int>(&block - s_block));
    }

    s_week_now = bare(grid);
    lv_obj_set_height(s_week_now, NOW_LINE_W);
    lv_obj_set_style_bg_opa(s_week_now, LV_OPA_COVER, 0);
    theme::fill_accent(s_week_now);
}

void week_step_clicked(lv_event_t *e)
{
    s_week_shift += static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    show_calendar();
}

lv_obj_t *week_chip(std::int32_t x, std::int32_t y, const char *glyph, int step)
{
    lv_obj_t *chip = theme::make_chip(s_agenda, glyph);
    lv_obj_set_pos(chip, x, y);
    lv_obj_set_ext_click_area(chip, space::s);
    lv_obj_add_event_cb(chip, week_step_clicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(step)));
    return chip;
}

void build_this_week_button(std::int32_t x, std::int32_t y)
{
    s_week_back = theme::make_button(s_agenda, "This week", theme::panel, theme::type_body());
    lv_obj_set_size(s_week_back, THIS_WEEK_W, theme::chip::size);
    lv_obj_set_style_radius(s_week_back, theme::radius::pill, 0);
    theme::set_text_color(lv_obj_get_child(s_week_back, 0), theme::secondary);
    lv_obj_set_pos(s_week_back, x, y);
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

void build_week_nav(std::int32_t height)
{
    const std::int32_t chip_y = height - space::l - theme::chip::inset - theme::chip::size;
    const std::int32_t start  = theme::chip::inset - space::l;
    week_chip(start, chip_y, LV_SYMBOL_LEFT, -1);
    s_week_name = line_label(s_agenda, theme::text, theme::type_body());
    lv_obj_set_width(s_week_name, DATES_W);
    lv_obj_set_style_text_align(s_week_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_week_name, start + theme::chip::size + space::s,
                   chip_y + (theme::chip::size - theme::type_body()->line_height) / 2);
    const std::int32_t next_x = start + theme::chip::size + 2 * space::s + DATES_W;
    week_chip(next_x, chip_y, LV_SYMBOL_RIGHT, 1);
    build_this_week_button(next_x + theme::chip::size + space::m, chip_y);
}

void build_week(lv_obj_t *parent, std::int32_t width, std::int32_t height)
{
    s_agenda = theme::make_card(parent);
    lv_obj_set_pos(s_agenda, 0, 0);
    lv_obj_set_size(s_agenda, width, height);
    lv_obj_set_hidden(s_agenda, true);
    quiet(s_agenda);

    const std::int32_t day_head_h = theme::type_label()->line_height + 2 * DAY_HEAD_PAD_V;
    const std::int32_t head_h     = day_head_h + space::m;
    build_week_heads(day_head_h);

    s_grid_w = width - 2 * space::l;
    s_grid_h = height - 2 * space::l - head_h - CHIP_CLEARANCE;
    build_week_grid(head_h);
    build_week_nav(height);
}

}  // namespace

void show_calendar()
{
    // Rebuilt every thirty seconds and on every calendar fetch; nobody needs
    // that while another page is up, and opening it draws it anew.
    if (s_title == nullptr || detail::s_page != detail::CALENDAR_PAGE) {
        return;
    }

    static ical::Event ahead[EVENTS_AHEAD];
    const int          count = ical::upcoming(ahead, EVENTS_AHEAD);
    const auto         now   = static_cast<std::int64_t>(std::time(nullptr));
    const ical::Event *next  = count > 0 ? &ahead[0] : nullptr;

    // Shown once asked for, which main does well before it starts.
    const bool want = next != nullptr && next->start > now && next->start - now < kJourneyAhead;

    pick_journey(want, now);
    show_next(next, now);
    show_day(next, now);
    place_left();
    if (std::exchange(s_day_relayout, false)) {
        show_day(next, now);
    }
    show_journey(next != nullptr ? next->start : 0, next != nullptr ? place_of(*next) : "");
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

    lv_obj_t *toggle = theme::make_chip(page, "");
    s_toggle_week    = theme::make_mark(toggle, &icons::calendar_icon);
    s_toggle_close   = theme::make_mark(toggle, &icons::times_icon);
    lv_obj_set_pos(toggle, width - theme::chip::inset - theme::chip::size,
                   height - theme::chip::inset - theme::chip::size);
    lv_obj_set_ext_click_area(toggle, space::m);
    lv_obj_add_event_cb(toggle, toggle_clicked, LV_EVENT_CLICKED, nullptr);
    build_detail(page);  // last, so it covers the page and the chip
    show_view();

    lv_timer_create([](lv_timer_t *) { show_calendar(); }, REFRESH_MS, nullptr);
    detail::subscribe(detail::Topic::Calendar, detail::kNoView, show_calendar);
}


// ---- Home's tile: what comes next, and the few after it ----
namespace {
constexpr int          TILE_AFTER = 4;
constexpr std::int32_t TILE_PAD   = 22;
constexpr std::int32_t TILE_WHEN_W = 150;
constexpr std::int32_t TILE_SIDE_BY_SIDE_W = 360;
constexpr std::int64_t TILE_REFRESH_S = 30;

struct TileRow {
    lv_obj_t *when;
    lv_obj_t *what;
};
lv_obj_t *s_tile       = nullptr;
lv_obj_t *s_tile_kind  = nullptr;
lv_obj_t *s_tile_title = nullptr;
lv_obj_t *s_tile_meta  = nullptr;
lv_obj_t *s_tile_big   = nullptr;
lv_obj_t *s_tile_span  = nullptr;
lv_obj_t *s_tile_after = nullptr;  // the heading over the rest
TileRow   s_tile_rows[TILE_AFTER]{};

// One line or two, as the title needs, so what is under it follows it closely.
void fit_tile_title()
{
    const lv_font_t   *font = fonts::size_28();
    const std::int32_t line = lv_font_get_line_height(font);
    lv_point_t         size{};
    lv_text_get_size(&size, lv_label_get_text(s_tile_title), font, 0, 0, lv_obj_get_width(s_tile_title),
                     LV_TEXT_FLAG_NONE);
    lv_obj_set_height(s_tile_title, size.y > line ? 2 * line : line);
}

lv_obj_t *tile_line(lv_obj_t *parent, std::uint32_t ink, const lv_font_t *font, std::int32_t w, int lines)
{
    lv_obj_t *label = theme::make_label(parent, "", ink, font);
    lv_obj_set_size(label, w, lines * lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(label, false);
    return label;
}

void show_tile()
{
    if (s_tile == nullptr) {
        return;
    }
    // Away with the calendar's own page while the phone is.
    lv_obj_set_hidden(s_tile, detail::s_presence_gate && !detail::status_state().present);
    if (detail::s_page != detail::HOME_PAGE) {
        return;
    }
    static ical::Event ahead[TILE_AFTER + 1];
    const int          count = ical::upcoming(ahead, TILE_AFTER + 1);
    const auto         now   = static_cast<std::int64_t>(std::time(nullptr));
    char               text[96];
    if (count == 0) {
        theme::set_text(s_tile_kind, "NEXT");
        theme::set_text(s_tile_title, "Nothing coming up");
        theme::set_text_color(s_tile_title, theme::secondary);
        fit_tile_title();
        for (lv_obj_t *part : {s_tile_meta, s_tile_big, s_tile_span, s_tile_after}) {
            theme::set_text(part, "");
        }
        for (TileRow &row : s_tile_rows) {
            theme::set_text(row.when, "");
            theme::set_text(row.what, "");
        }
        return;
    }
    const ical::Event &next    = ahead[0];
    const bool         ongoing = next.start <= now;
    char               day[24];
    day_name(now, next.start, day, sizeof(day));
    std::snprintf(text, sizeof(text), "%s, %s", feed_kind(next.feed), ongoing ? "now" : day);
    theme::set_text(s_tile_kind, text);
    theme::set_text(s_tile_title, next.summary);
    theme::set_text_color(s_tile_title, theme::text);
    fit_tile_title();
    char from[16];
    char to[16];
    clock_of(next.start, from, sizeof(from));
    clock_of(next.end, to, sizeof(to));
    if (has_place(next)) {
        std::snprintf(text, sizeof(text), "%s \xe2\x80\x93 %s, %s", from, to, place_of(next));
    } else {
        std::snprintf(text, sizeof(text), "%s \xe2\x80\x93 %s", from, to);
    }
    theme::set_text(s_tile_meta, text);
    theme::set_text(s_tile_big, ongoing ? to : from);
    char span[32];
    span_of(now, ongoing ? next.end : next.start, span, sizeof(span));
    std::snprintf(text, sizeof(text), "%s %s", ongoing ? "ends" : "", span);
    theme::set_text(s_tile_span, ongoing ? text : span);

    theme::set_text(s_tile_after, count > 1 ? "AFTER THAT" : "");
    for (int i = 0; i < TILE_AFTER; ++i) {
        TileRow &row = s_tile_rows[i];
        if (i + 1 >= count) {
            theme::set_text(row.when, "");
            theme::set_text(row.what, "");
            continue;
        }
        const ical::Event &event = ahead[i + 1];
        const auto         at    = static_cast<std::time_t>(event.start);
        std::tm            local{};
        localtime_r(&at, &local);
        std::strftime(text, sizeof(text), "%a %H:%M", &local);
        theme::set_text(row.when, text);
        theme::set_text(row.what, event.summary);
    }
}
}  // namespace

void build_next_tile(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
{
    s_tile = lv_button_create(parent);
    theme::style_button(s_tile, theme::panel_light);
    lv_obj_set_pos(s_tile, x, y);
    lv_obj_set_size(s_tile, w, h);
    lv_obj_set_style_radius(s_tile, theme::radius::card, 0);
    lv_obj_set_style_pad_all(s_tile, TILE_PAD, 0);
    lv_obj_set_scrollable(s_tile, false);
    lv_obj_add_event_cb(s_tile, [](lv_event_t *) { detail::select_page(detail::CALENDAR_PAGE); }, LV_EVENT_CLICKED,
                        nullptr);
    const std::int32_t inner = w - 2 * TILE_PAD;
    s_tile_kind  = tile_line(s_tile, theme::secondary, theme::type_label(), inner, 1);
    s_tile_title = tile_line(s_tile, theme::text, fonts::size_28(), inner, 2);
    s_tile_meta  = tile_line(s_tile, theme::secondary, theme::type_body(), inner, 1);
    // The time and how far off it is, the one after the other whatever their widths.
    lv_obj_t *when = lv_obj_create(s_tile);
    lv_obj_remove_style_all(when);
    lv_obj_set_size(when, inner, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(when, LV_FLEX_FLOW_ROW_WRAP);  // how far off under the time, where there is no room beside it
    lv_obj_set_flex_align(when, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(when, space::l, 0);
    lv_obj_set_clickable(when, false);
    s_tile_big  = theme::make_label(when, "", theme::text, fonts::size_48());
    s_tile_span = theme::make_label(when, "", theme::secondary, theme::type_body());
    lv_obj_set_style_pad_bottom(s_tile_span, space::s, 0);
    // What it is, when and where, and the time, each under the last.
    lv_obj_t *head = lv_obj_create(s_tile);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, inner, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, space::s, 0);
    lv_obj_set_clickable(head, false);
    for (lv_obj_t *part : {s_tile_kind, s_tile_title, s_tile_meta, when}) {
        lv_obj_set_parent(part, head);
    }

    s_tile_after = theme::make_eyebrow(s_tile, "");
    lv_obj_align(s_tile_after, LV_ALIGN_TOP_LEFT, 0, 228);
    // Narrow, when over what; wide enough, the two side by side.
    const bool         stacked = inner < TILE_SIDE_BY_SIDE_W;
    const std::int32_t text    = lv_font_get_line_height(theme::type_body());
    const std::int32_t line    = (stacked ? 2 * text : text) + space::m;
    const std::int32_t when_w  = stacked ? inner : TILE_WHEN_W;
    for (int i = 0; i < TILE_AFTER; ++i) {
        TileRow &row = s_tile_rows[i];
        row.when     = tile_line(s_tile, theme::secondary, theme::type_body(), when_w, 1);
        row.what     = tile_line(s_tile, theme::text, theme::type_body(), stacked ? inner : inner - TILE_WHEN_W, 1);
        const std::int32_t ry = 258 + i * line;
        if (ry + line > h - 2 * TILE_PAD) {
            lv_obj_set_hidden(row.when, true);
            lv_obj_set_hidden(row.what, true);
        }
        lv_obj_align(row.when, LV_ALIGN_TOP_LEFT, 0, ry);
        lv_obj_align(row.what, LV_ALIGN_TOP_LEFT, stacked ? 0 : TILE_WHEN_W, stacked ? ry + text : ry);
    }
    detail::subscribe(detail::Topic::Calendar, detail::kNoView, show_tile);
    detail::subscribe(detail::Topic::Page, detail::kNoView, show_tile);
    detail::subscribe(detail::Topic::Status, detail::kNoView, show_tile);
    detail::subscribe(detail::Topic::Second, detail::kNoView, [] {
        if (std::time(nullptr) % TILE_REFRESH_S == 0) {
            show_tile();
        }
    });
}

}  // namespace ui
