#include "calendar_page.h"

#include "ui_internal.h"
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
constexpr int TITLE_LINES_MAX = 2;
constexpr int ALSO_ITEMS      = 3;  // the rest of the next event's day, under it

// The hours the strip and the week show however little is on them.
constexpr float WORKDAY_FIRST_HOUR = 8.0f;
constexpr float WORKDAY_LAST_HOUR  = 18.0f;
// Over this many hours, a mark an hour is a grid, so they go every other hour.
constexpr int HOURLY_MARKS_MAX = 8;
constexpr int SPARSE_MARK_STEP = 2;

constexpr std::int64_t SHOW_WAYS_WITHIN = 3 * units::kSecondsPerHour;  // hours off is nothing to act on yet

constexpr std::int32_t LIST_W        = 500;
constexpr std::int32_t DOT           = 10;
constexpr std::int32_t TIME_W        = 64;
constexpr std::int32_t BASELINE_LIFT = 6;  // a smaller line beside a large one, onto its baseline
constexpr std::int32_t SCROLLBAR_W   = 4;
constexpr std::int32_t NOW_LINE_W    = 2;
// How far a corner chip reaches up into a card's padding, with a gap over it.
constexpr std::int32_t CHIP_CLEARANCE =
    theme::chip::size + theme::chip::inset - space::l + space::s;

constexpr std::int32_t BAR_W         = 4;   // an event's colour, beside its words
constexpr std::int32_t STEP_TIME_W   = 64;
constexpr std::int32_t STEP_ICON     = 40;  // the square a step's icon sits in
constexpr std::int32_t CHOICE_CHIP_H = 40;
constexpr std::int64_t LONG_EVENT    = 4 * units::kSecondsPerHour;  // said with its end, as a day at work

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

struct Item {  // one row of the agenda, or of the next event's day: a day's name, or an event
    lv_obj_t *root  = nullptr;
    lv_obj_t *time  = nullptr;
    lv_obj_t *bar   = nullptr;  // in the feed's colour
    lv_obj_t *title = nullptr;
    lv_obj_t *place = nullptr;
    lv_obj_t *date  = nullptr;  // a day's date, at the right
};

struct Step {  // one thing to do on the way there
    lv_obj_t *root   = nullptr;
    lv_obj_t *time   = nullptr;
    lv_obj_t *icon   = nullptr;
    lv_obj_t *what   = nullptr;
    lv_obj_t *detail = nullptr;
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
lv_obj_t *s_session   = nullptr;  // the part of the title after " - "
lv_obj_t *s_until     = nullptr;  // beside the start, when it ends
lv_obj_t *s_place     = nullptr;
lv_obj_t *s_next      = nullptr;
lv_obj_t *s_after     = nullptr;  // the rest of the week
std::int32_t s_page_h  = 0;

lv_obj_t *s_foot      = nullptr;  // the way there, or the rest of the day
lv_obj_t *s_foot_head = nullptr;
lv_obj_t *s_leave     = nullptr;
lv_obj_t *s_there     = nullptr;
lv_obj_t *s_spare     = nullptr;
lv_obj_t *s_leave_row = nullptr;
Step      s_step[travel::kLegsMax];
lv_obj_t *s_choices   = nullptr;  // the other times to leave
lv_obj_t *s_choice[travel::kOptionsMax];
Item      s_also[ALSO_ITEMS];       // the rest of the next event's day

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

// Where the walk at the end goes, which the backend leaves to be named: the
// room the event is in, or just there.
const char *end_name(const char *place)
{
    return place != nullptr && place[0] != '\0' ? place : "There";
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

// What kind of event, and when: how long until it, or until it ends, on its
// day, and which day before that.
void kind_text(const ical::Event &event, std::int64_t now, char *out, std::size_t size)
{
    char when[32];
    if (event.start <= now) {
        span_of(now, event.end, when, sizeof(when));
        std::snprintf(out, size, "%s, ends %s", feed_kind(event.feed), when);
        return;
    }
    if (days_from(now, event.start) < 1) {
        span_of(now, event.start, when, sizeof(when));
    } else {
        day_name(now, event.start, when, sizeof(when));
    }
    std::snprintf(out, size, "%s, %s", feed_kind(event.feed), when);
}

// A timetable's title is the course and the session, "Course - Session": each
// gets a line of its own rather than both being cut off.
void split_title(const char *summary, char *course, std::size_t course_size, char *session,
                 std::size_t session_size)
{
    const char *dash = std::strstr(summary, " - ");
    if (dash == nullptr) {
        std::snprintf(course, course_size, "%s", summary);
        session[0] = '\0';
        return;
    }
    std::snprintf(course, course_size, "%.*s", static_cast<int>(dash - summary), summary);
    std::snprintf(session, session_size, "%s", dash + 3);
}

bool is_digit(char c)
{
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool is_upper(char c)
{
    return std::isupper(static_cast<unsigned char>(c)) != 0;
}

// An address as street and town: "Voorbeeldstraat 12 1234AB Plaats Nederland" is
// Voorbeeldstraat 12, Plaats. A room is left as it is.
void short_place(const char *place, char *out, std::size_t size)
{
    std::snprintf(out, size, "%s", place);
    for (char *at = out; at[0] != '\0'; ++at) {
        const bool postcode = (at == out || at[-1] == ' ') && is_digit(at[0]) && is_digit(at[1]) &&
                              is_digit(at[2]) && is_digit(at[3]) && is_upper(at[4]) && is_upper(at[5]);
        if (!postcode) {
            continue;
        }
        char rest[64];
        std::snprintf(rest, sizeof(rest), "%s", at + 6 + (at[6] == ' ' ? 1 : 0));
        // The country after the town, whole or as far as the location was kept.
        if (char *last = std::strrchr(rest, ' '); last != nullptr && std::strlen(last + 1) >= 2 &&
                                                  (std::strncmp("Nederland", last + 1, std::strlen(last + 1)) == 0 ||
                                                   std::strncmp("Netherlands", last + 1, std::strlen(last + 1)) == 0)) {
            *last = '\0';
        }
        while (at > out && at[-1] == ' ') {
            --at;
        }
        std::snprintf(at, size - static_cast<std::size_t>(at - out), ", %s", rest);
        return;
    }
}

// An event's line in a list: its course, and for a long one, until when.
void list_title(const ical::Event &event, char *out, std::size_t size)
{
    char course[ical::kSummaryMax];
    char session[ical::kSummaryMax];
    split_title(event.summary, course, sizeof(course), session, sizeof(session));
    if (event.end - event.start >= LONG_EVENT) {
        char until[16];
        clock_of(event.end, until, sizeof(until));
        std::snprintf(out, size, "%s, until %s", course, until);
    } else {
        std::snprintf(out, size, "%s", course);
    }
}

// And under it, the session and the place.
void list_place(const ical::Event &event, char *out, std::size_t size)
{
    char course[ical::kSummaryMax];
    char session[ical::kSummaryMax];
    char place[64];
    split_title(event.summary, course, sizeof(course), session, sizeof(session));
    short_place(place_of(event), place, sizeof(place));
    if (session[0] != '\0' && place[0] != '\0') {
        std::snprintf(out, size, "%s, %s", session, place);
    } else {
        std::snprintf(out, size, "%s", session[0] != '\0' ? session : place);
    }
}

void show_item_event(Item &item, const ical::Event &event, const char *note)
{
    char text[ical::kSummaryMax + 96];  // a title or a session with its place, whole
    clock_of(event.start, text, sizeof(text));
    lv_obj_set_hidden(item.time, false);
    lv_obj_set_hidden(item.bar, false);
    lv_obj_set_hidden(item.date, true);
    theme::set_text(item.time, text);
    lv_obj_set_style_bg_color(item.bar, lv_color_hex(feed_ink(event.feed)), 0);
    list_title(event, text, sizeof(text));
    theme::set_text(item.title, text);
    theme::set_text_color(item.title, theme::text);
    lv_obj_set_style_text_font(item.title, theme::type_body(), 0);
    lv_obj_set_height(item.title, theme::type_body()->line_height);
    if (note != nullptr) {
        theme::set_text(item.place, note);
    } else {
        list_place(event, text, sizeof(text));
        theme::set_text(item.place, text);
    }
    lv_obj_set_hidden(item.place, lv_label_get_text(item.place)[0] == '\0');
    lv_obj_set_style_margin_top(item.root, 0, 0);
    lv_obj_set_hidden(item.root, false);
}

// What else is on the next event's day, under it, a clash said in words.
void show_also(const ical::Event *ahead, int count, std::int64_t now)
{
    const ical::Event &next  = ahead[0];
    const int          day   = days_from(now, next.start);
    int                shown = 0;
    for (int i = 1; i < count && shown < ALSO_ITEMS; ++i) {
        if (days_from(now, ahead[i].start) != day) {
            break;
        }
        char note[96];
        char place[64];
        short_place(place_of(ahead[i]), place, sizeof(place));
        const bool clash = ahead[i].start < next.end;
        std::snprintf(note, sizeof(note), "%s%s%s%s", feed_kind(ahead[i].feed), place[0] != '\0' ? ", " : "", place,
                      clash ? ", at the same time" : "");
        show_item_event(s_also[shown++], ahead[i], note);
    }
    for (int i = shown; i < ALSO_ITEMS; ++i) {
        lv_obj_set_hidden(s_also[i].root, true);
    }
    char name[24];
    day_name(now, next.start, name, sizeof(name));
    char head[40];
    std::snprintf(head, sizeof(head), "Also %s%s", day <= 1 ? "" : "on ", name);
    for (char *c = head; *c != '\0'; ++c) {
        *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    }
    theme::set_text(s_foot_head, head);
    lv_obj_set_hidden(s_foot, shown == 0);
}

// What to do at a step: the train and where it goes, or where to walk or ride to.
void step_text(const travel::Leg &leg, bool last, const char *place, char *what, std::size_t what_size,
               char *detail, std::size_t detail_size)
{
    const int   mins  = minutes_rounded_up(leg.arrive - leg.depart);
    const char *about = leg.estimated ? "about " : "";
    const char *to    = last ? end_name(place) : leg.to;
    char        out_at[16];
    clock_of(leg.arrive, out_at, sizeof(out_at));
    if (is_walk_or_bike(leg.mode)) {
        std::snprintf(what, what_size, "%s to %s", is_mode(leg.mode, "walk") ? "Walk" : "Bike", to);
        std::snprintf(detail, detail_size, "%s%d min", about, mins);
    } else {
        char        kind[travel::kLineMax];
        const char *plus = std::strstr(leg.line, CHANGES_MARK);
        std::snprintf(kind, sizeof(kind), "%.*s",
                      static_cast<int>(plus != nullptr ? plus - leg.line : std::strlen(leg.line)), leg.line);
        if (is_mode(leg.mode, "train")) {
            std::snprintf(what, what_size, "%s to %s", kind[0] != '\0' ? line_name(kind) : "Train", leg.to);
        } else {
            std::snprintf(what, what_size, "Bus%s%s to %s", kind[0] != '\0' ? " " : "", kind, leg.to);
        }
        const int changes = plus != nullptr ? std::atoi(plus + std::strlen(CHANGES_MARK)) : 0;
        if (changes > 0) {
            std::snprintf(detail, detail_size, "from %s, %d change%s, out at %s", leg.from, changes,
                          changes == 1 ? "" : "s", out_at);
        } else {
            std::snprintf(detail, detail_size, "from %s, out at %s", leg.from, out_at);
        }
    }
    if (leg.cancelled) {
        std::snprintf(detail, detail_size, "cancelled");
    }
}

// The other times to leave, a tap taking one.
void show_choices(std::int64_t starts)
{
    const bool pick = s_way_count > 1;
    lv_obj_set_hidden(s_choices, !pick);
    for (int i = 0; i < travel::kOptionsMax; ++i) {
        const bool real = pick && i < s_way_count;
        lv_obj_set_hidden(s_choice[i], !real);
        if (!real) {
            continue;
        }
        const travel::Option &way = s_ways[s_way_order[i]];
        char                  text[32];
        clock_of(way.leave, text, sizeof(text));
        if (way.late && starts > 0) {
            const std::size_t at = std::strlen(text);
            std::snprintf(text + at, sizeof(text) - at, ", %d min late", minutes_rounded_up(way.arrive - starts));
        }
        lv_obj_t *label = lv_obj_get_child(s_choice[i], 0);
        theme::set_text(label, text);
        const bool on = &way == s_going;
        lv_obj_set_state(s_choice[i], LV_STATE_CHECKED, on);
        theme::set_text_color(label, on            ? theme::text
                                     : way.cancelled ? theme::red
                                     : way.late      ? theme::amber
                                                     : theme::secondary);
    }
}

// The way there as what to do: when to leave, then each step.
void show_journey(std::int64_t starts, const char *place)
{
    char text[64];
    theme::set_text(s_foot_head, s_going->cancelled ? "LEAVE AT, CANCELLED" : "LEAVE AT");
    clock_of(s_going->leave, text, sizeof(text));
    theme::set_text(s_leave, text);
    theme::set_text_color(s_leave, s_going->cancelled ? theme::red : theme::primary);
    char there[sizeof(text) + 8];
    clock_of(s_going->arrive, text, sizeof(text));
    std::snprintf(there, sizeof(there), "there %s", text);
    theme::set_text(s_there, there);
    const int spare = whole_minutes(starts - s_going->arrive);
    std::snprintf(text, sizeof(text), "%d min %s", spare >= 0 ? spare : -spare, spare >= 0 ? "early" : "late");
    theme::set_text(s_spare, text);
    theme::set_text_color(s_spare, spare >= 0 ? theme::green : theme::amber);

    for (int i = 0; i < travel::kLegsMax; ++i) {
        Step      &step = s_step[i];
        const bool real = i < s_going->leg_count;
        lv_obj_set_hidden(step.root, !real);
        if (!real) {
            continue;
        }
        const travel::Leg &leg = s_going->legs[i];
        clock_of(leg.depart, text, sizeof(text));
        theme::set_text(step.time, text);
        theme::set_text_color(step.time, leg.cancelled   ? theme::red
                                         : leg.estimated ? theme::secondary
                                                         : theme::text);
        lv_image_set_src(step.icon, icon_of(leg.mode));
        lv_obj_set_style_image_recolor(step.icon, lv_color_hex(leg.cancelled ? theme::red : theme::text), 0);
        char what[96];
        char detail[96];
        step_text(leg, i == s_going->leg_count - 1, place, what, sizeof(what), detail, sizeof(detail));
        theme::set_text(step.what, what);
        theme::set_text(step.detail, detail);
        theme::set_text_color(step.detail, leg.cancelled ? theme::red : theme::secondary);
    }
    show_choices(starts);
    lv_obj_set_hidden(s_foot, false);
}

// Under the next event: the way there while there is one, else the rest of its day.
void show_foot(const ical::Event *ahead, int count, std::int64_t now)
{
    const bool way = s_going != nullptr && count > 0;
    lv_obj_set_hidden(s_leave_row, !way);
    for (Step &step : s_step) {
        lv_obj_set_hidden(step.root, true);
    }
    lv_obj_set_hidden(s_choices, true);
    for (Item &item : s_also) {
        lv_obj_set_hidden(item.root, true);
    }
    if (way) {
        show_journey(ahead[0].start, place_of(ahead[0]));
        // A long route takes the session's line.
        if (s_going->leg_count > 3) {
            lv_obj_set_hidden(s_session, true);
        }
    } else if (count > 0) {
        show_also(ahead, count, now);
    } else {
        lv_obj_set_hidden(s_foot, true);
    }
}

void show_next(const ical::Event *first, std::int64_t now)
{
    if (first == nullptr) {
        lv_obj_set_hidden(s_kind_dot, true);
        theme::set_text(s_kind, "");
        theme::set_text(s_title, "Nothing coming up");
        fit_lines(s_title, s_title_w, TITLE_LINES_MAX);
        lv_obj_set_hidden(s_session, true);
        theme::set_text(s_meta, "");
        theme::set_text(s_until, "");
        theme::set_text(s_place, "");
        return;
    }
    char text[ical::kSummaryMax];
    lv_obj_set_hidden(s_kind_dot, false);
    lv_obj_set_style_bg_color(s_kind_dot, lv_color_hex(feed_ink(first->feed)), 0);
    kind_text(*first, now, text, sizeof(text));
    theme::set_text(s_kind, text);

    char session[ical::kSummaryMax];
    split_title(first->summary, text, sizeof(text), session, sizeof(session));
    theme::set_text(s_title, text);
    fit_lines(s_title, s_title_w, TITLE_LINES_MAX);
    theme::set_text(s_session, session);
    lv_obj_set_hidden(s_session, session[0] == '\0');

    char from[16];
    char to[24];
    clock_of(first->start, from, sizeof(from));
    theme::set_text(s_meta, from);
    clock_of(first->end, text, sizeof(text));
    std::snprintf(to, sizeof(to), "\xe2\x80\x93 %.16s", text);
    theme::set_text(s_until, to);
    short_place(place_of(*first), text, sizeof(text));
    theme::set_text(s_place, text);
    lv_obj_set_hidden(s_place, text[0] == '\0');
}

void show_day_heading(Item &head, std::int64_t now, std::int64_t at, bool first)
{
    char name[24];
    day_name(now, at, name, sizeof(name));
    const std::tm when = local(at);
    char          date[16];
    std::strftime(date, sizeof(date), "%e %b", &when);
    lv_obj_set_hidden(head.time, true);
    lv_obj_set_hidden(head.bar, true);
    lv_obj_set_hidden(head.place, true);
    lv_obj_set_hidden(head.date, false);
    theme::set_text(head.title, name);
    theme::set_text(head.date, date[0] == ' ' ? date + 1 : date);
    theme::set_text_color(head.title, theme::secondary);
    lv_obj_set_style_text_font(head.title, theme::type_label(), 0);
    lv_obj_set_height(head.title, theme::type_label()->line_height);
    lv_obj_set_style_margin_top(head.root, first ? 0 : space::s, 0);
    lv_obj_set_hidden(head.root, false);
}

void show_nothing_listed(Item &none)
{
    lv_obj_set_hidden(none.time, true);
    lv_obj_set_hidden(none.bar, true);
    lv_obj_set_hidden(none.date, true);
    theme::set_text(none.title, "Nothing else in the coming week");
    theme::set_text_color(none.title, theme::secondary);
    lv_obj_set_style_text_font(none.title, theme::type_body(), 0);
    lv_obj_set_height(none.title, theme::type_body()->line_height);
    lv_obj_set_hidden(none.place, true);
    lv_obj_set_hidden(none.root, false);
}

// The coming seven days, rather than what is left of the calendar week, which
// on a Friday is nothing; the next event's day is under it unless the way
// there is.
void show_list(const ical::Event *ahead, int count, std::int64_t now)
{
    const std::int64_t week_end = now + SECONDS_PER_WEEK;
    const int          next_day = count > 0 ? days_from(now, ahead[0].start) : -1;
    const bool         skip     = s_going == nullptr;
    int                used     = 0;
    int                last_day = -1;
    for (int i = 1; i < count && used < LIST_ITEMS && ahead[i].start < week_end; ++i) {
        const int day = days_from(now, ahead[i].start);
        if (skip && day == next_day) {
            continue;
        }
        if (day != last_day) {
            if (used + 1 >= LIST_ITEMS) {
                break;  // a day's name with nothing under it
            }
            show_day_heading(s_item[used], now, ahead[i].start, used == 0);
            ++used;
            last_day = day;
        }
        show_item_event(s_item[used++], ahead[i], nullptr);
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
    // The course alone, as the agenda has it: the session is a tap away.
    char course[ical::kSummaryMax];
    char session[ical::kSummaryMax];
    split_title(event.summary, course, sizeof(course), session, sizeof(session));
    theme::set_text(block.title, course);
    theme::set_text_color(block.title, over ? theme::secondary : theme::text);

    // The room under the title, when a line is left over for it.
    char place[64];
    short_place(place_of(event), place, sizeof(place));
    const char        *where  = place;
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

void choice_clicked(lv_event_t *e)
{
    const auto index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (index < s_way_count) {
        s_picked_leave = s_ways[s_way_order[index]].leave;
        show_calendar();
    }
}

// A row of a list: the time, the event's colour, its title and place; or a
// day's name with its date at the right.
void build_item(Item &item, lv_obj_t *parent)
{
    const std::int32_t line = theme::type_body()->line_height;
    item.root = row_of(parent, LV_SIZE_CONTENT, space::m);
    lv_obj_set_flex_align(item.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    item.time = line_label(item.root, theme::text, theme::type_body());
    lv_obj_set_width(item.time, TIME_W);
    item.bar = bare(item.root);
    lv_obj_set_size(item.bar, BAR_W, line + theme::type_label()->line_height);
    lv_obj_set_style_radius(item.bar, BAR_W / 2, 0);
    lv_obj_set_style_bg_opa(item.bar, LV_OPA_COVER, 0);
    lv_obj_t *what = column_of(item.root, 0);
    lv_obj_set_width(what, 0);
    lv_obj_set_flex_grow(what, 1);
    item.title = dotted_label(what, theme::text, theme::type_body());
    lv_obj_set_width(item.title, LV_PCT(100));
    item.place = line_label(what, theme::secondary, theme::type_label());
    lv_obj_set_width(item.place, LV_PCT(100));
    item.date = line_label(item.root, theme::secondary, theme::type_label());
    lv_obj_set_hidden(item.root, true);
}

// One step of the way there: its time, what it is by, and what to do.
void build_step(Step &step)
{
    step.root = row_of(s_foot, LV_SIZE_CONTENT, space::m);
    step.time = line_label(step.root, theme::text, theme::type_body());
    lv_obj_set_width(step.time, STEP_TIME_W);
    lv_obj_t *tile = bare(step.root);
    lv_obj_set_size(tile, STEP_ICON, STEP_ICON);
    theme::style_panel(tile, theme::panel, theme::radius::control);
    step.icon = lv_image_create(tile);
    lv_obj_center(step.icon);
    lv_obj_set_style_image_recolor_opa(step.icon, LV_OPA_COVER, 0);
    quiet(step.icon);
    lv_obj_t *words = column_of(step.root, 0);
    lv_obj_set_width(words, 0);
    lv_obj_set_flex_grow(words, 1);
    step.what   = line_label(words, theme::text, theme::type_body());
    lv_obj_set_width(step.what, LV_PCT(100));
    step.detail = line_label(words, theme::secondary, theme::type_label());
    lv_obj_set_width(step.detail, LV_PCT(100));
    lv_obj_set_hidden(step.root, true);
}

void build_choices()
{
    s_choices = row_of(s_foot, CHOICE_CHIP_H, space::s);
    lv_obj_t *lead = line_label(s_choices, theme::secondary, theme::type_label());
    theme::set_text(lead, "Or leave at");
    for (int i = 0; i < travel::kOptionsMax; ++i) {
        lv_obj_t *chip = lv_button_create(s_choices);
        theme::style_button(chip, theme::panel);
        theme::fill_accent(chip, LV_STATE_CHECKED);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, CHOICE_CHIP_H);
        lv_obj_set_style_radius(chip, theme::radius::pill, 0);
        lv_obj_set_style_pad_hor(chip, space::m, 0);
        lv_obj_t *label = line_label(chip, theme::secondary, theme::type_label());
        lv_obj_center(label);
        lv_obj_add_event_cb(chip, choice_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        s_choice[i] = chip;
    }
    lv_obj_set_hidden(s_choices, true);
}

// Under the next event, set off by a line: the way there, or the rest of its day.
void build_foot()
{
    s_foot = column_of(s_next, space::s);
    lv_obj_set_style_border_side(s_foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_foot, 1, 0);
    lv_obj_set_style_border_color(s_foot, lv_color_hex(theme::panel), 0);
    lv_obj_set_style_pad_top(s_foot, space::m, 0);
    s_foot_head = theme::make_eyebrow(s_foot, "");
    quiet(s_foot_head);

    s_leave_row = row_of(s_foot, LV_SIZE_CONTENT, space::m);
    lv_obj_set_flex_align(s_leave_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    s_leave = line_label(s_leave_row, theme::primary, theme::type_display());
    lv_obj_t *there = column_of(s_leave_row, 0);
    lv_obj_set_width(there, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(there, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    s_there = line_label(there, theme::text, theme::type_value());
    s_spare = line_label(there, theme::green, theme::type_label());

    for (Step &step : s_step) {
        build_step(step);
    }
    build_choices();
    for (Item &item : s_also) {
        build_item(item, s_foot);
    }
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
    s_session = line_label(s_next, theme::secondary, theme::type_body());
    lv_obj_set_width(s_session, LV_PCT(100));

    // When and where, what is needed to get there on time, large.
    lv_obj_t *when = row_of(s_next, LV_SIZE_CONTENT, space::s);
    lv_obj_set_style_margin_top(when, space::m, 0);
    lv_obj_set_flex_align(when, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    s_meta  = line_label(when, theme::text, theme::type_display());
    s_until = line_label(when, theme::secondary, theme::type_value());
    lv_obj_set_style_margin_bottom(s_until, BASELINE_LIFT, 0);
    s_place = line_label(s_next, theme::text, theme::type_title());
    lv_obj_set_width(s_place, LV_PCT(100));

    lv_obj_t *give = bare(s_next);  // the foot at the card's bottom
    lv_obj_set_size(give, 1, 1);
    lv_obj_set_flex_grow(give, 1);
    build_foot();
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

// The rest of the week as an agenda, clear of the corner chip.
void build_coming_up(std::int32_t x)
{
    s_after = theme::make_card(s_overview);
    lv_obj_set_pos(s_after, x, 0);
    lv_obj_set_size(s_after, LIST_W, s_page_h);
    quiet(s_after);

    const std::int32_t list_h = s_page_h - 2 * space::l - theme::chip::size;
    s_list = column_of(s_after, space::s);
    lv_obj_set_height(s_list, list_h);
    lv_obj_set_clickable(s_list, true);
    lv_obj_set_scrollable(s_list, true);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(theme::secondary), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_list, SCROLLBAR_W, LV_PART_SCROLLBAR);
    for (Item &item : s_item) {
        build_item(item, s_list);
    }

    // The list scrolls on below; its last lines fade into the card, rather than
    // stop at a day's name whose events are out of sight.
    lv_obj_t *fade = lv_obj_create(s_after);
    lv_obj_set_size(fade, LIST_W - 2 * space::l, LIST_FADE_H);
    lv_obj_set_pos(fade, 0, list_h - LIST_FADE_H);
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
    build_next_card(width - LIST_W - space::m);
    build_coming_up(width - LIST_W);
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
    show_foot(ahead, count, now);
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
constexpr int          TILE_AFTER    = 4;
constexpr std::int32_t TILE_PAD      = 22;
constexpr std::int32_t TILE_WHEN_W   = 150;
constexpr std::int32_t TILE_SIDE_W   = 360;  // narrower, when stands over what rather than beside it

struct TileRow {
    lv_obj_t *row;
    lv_obj_t *when;
    lv_obj_t *what;
};
lv_obj_t    *s_tile       = nullptr;
std::int32_t s_tile_w     = 0;  // inside its padding
lv_obj_t    *s_tile_kind  = nullptr;
lv_obj_t    *s_tile_when  = nullptr;
lv_obj_t    *s_tile_big   = nullptr;
lv_obj_t    *s_tile_until = nullptr;
lv_obj_t    *s_tile_place = nullptr;
lv_obj_t    *s_tile_title = nullptr;
lv_obj_t    *s_tile_after = nullptr;  // the heading over the rest
TileRow      s_tile_rows[TILE_AFTER]{};

void show_tile()
{
    if (s_tile == nullptr) {
        return;
    }
    // Away with the calendar's own page while the phone is.
    lv_obj_set_hidden(s_tile, detail::owner_away());
    if (detail::s_page != detail::HOME_PAGE) {
        return;
    }
    static ical::Event ahead[TILE_AFTER + 1];
    const int          count = ical::upcoming(ahead, TILE_AFTER + 1);
    const auto         now   = static_cast<std::int64_t>(std::time(nullptr));
    char               text[96];
    const bool         any   = count > 0;
    const ical::Event &next  = ahead[0];
    if (any) {
        kind_text(next, now, text, sizeof(text));
    }
    theme::set_text(s_tile_kind, any ? text : "NEXT");
    lv_obj_set_hidden(s_tile_when, !any);
    lv_obj_set_hidden(s_tile_place, true);
    if (any) {
        clock_of(next.start, text, sizeof(text));
        theme::set_text(s_tile_big, text);
        char until[24];
        clock_of(next.end, text, sizeof(text));
        std::snprintf(until, sizeof(until), "\xe2\x80\x93 %.16s", text);
        theme::set_text(s_tile_until, until);
        short_place(place_of(next), text, sizeof(text));
        theme::set_text(s_tile_place, text);
        lv_obj_set_hidden(s_tile_place, text[0] == '\0');
    }
    // The course alone, as the calendar's agenda has it.
    char course[ical::kSummaryMax];
    char session[ical::kSummaryMax];
    split_title(any ? next.summary : "", course, sizeof(course), session, sizeof(session));
    theme::set_text(s_tile_title, any ? course : "Nothing coming up");
    theme::set_text_color(s_tile_title, any ? theme::text : theme::secondary);
    fit_lines(s_tile_title, s_tile_w, 3);

    for (int i = 0; i < TILE_AFTER; ++i) {
        TileRow &row = s_tile_rows[i];
        lv_obj_set_hidden(row.row, i + 1 >= count);
        if (i + 1 >= count) {
            continue;
        }
        const ical::Event &event = ahead[i + 1];
        const auto         at    = static_cast<std::time_t>(event.start);
        std::tm            local{};
        localtime_r(&at, &local);
        std::strftime(text, sizeof(text), "%a %H:%M", &local);
        theme::set_text(row.when, text);
        split_title(event.summary, course, sizeof(course), session, sizeof(session));
        theme::set_text(row.what, course);
    }
    // Only whole rows: one the card would cut through goes, and those after it.
    lv_obj_set_hidden(s_tile_after, false);
    lv_obj_update_layout(s_tile);
    lv_area_t tile;
    lv_obj_get_coords(s_tile, &tile);
    int shown = 0;
    for (int i = 0; i < TILE_AFTER; ++i) {
        lv_area_t area;
        lv_obj_get_coords(s_tile_rows[i].row, &area);
        if (shown < i || area.y2 > tile.y2 - TILE_PAD) {
            lv_obj_set_hidden(s_tile_rows[i].row, true);
        }
        shown += lv_obj_is_hidden(s_tile_rows[i].row) ? 0 : 1;
    }
    lv_obj_set_hidden(s_tile_after, shown == 0);
}
}  // namespace

// One column, each part under the last however many lines it takes, the card
// clipping whatever of the rest has no room.
void build_next_tile(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
{
    s_tile = lv_button_create(parent);
    theme::style_button(s_tile, theme::panel_light);
    lv_obj_set_pos(s_tile, x, y);
    lv_obj_set_size(s_tile, w, h);
    lv_obj_set_style_radius(s_tile, theme::radius::card, 0);
    lv_obj_set_style_pad_all(s_tile, TILE_PAD, 0);
    lv_obj_set_scrollable(s_tile, false);
    lv_obj_set_flex_flow(s_tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_tile, space::s, 0);
    lv_obj_add_event_cb(s_tile, [](lv_event_t *) { detail::select_page(detail::CALENDAR_PAGE); }, LV_EVENT_CLICKED,
                        nullptr);
    s_tile_w     = w - 2 * TILE_PAD;
    s_tile_kind = line_label(s_tile, theme::secondary, theme::type_label());
    // When it starts and where, large; the end drops under the start when there is no room beside it.
    s_tile_when = row_of(s_tile, LV_SIZE_CONTENT, space::s);
    lv_obj_set_width(s_tile_when, s_tile_w);
    lv_obj_set_flex_flow(s_tile_when, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_tile_when, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    s_tile_big   = line_label(s_tile_when, theme::text, theme::type_display());
    s_tile_until = line_label(s_tile_when, theme::secondary, theme::type_body());
    lv_obj_set_style_margin_bottom(s_tile_until, BASELINE_LIFT, 0);
    s_tile_place = line_label(s_tile, theme::text, theme::type_title());
    s_tile_title = dotted_label(s_tile, theme::text, theme::type_value());
    lv_obj_set_style_margin_top(s_tile_title, space::xs, 0);
    for (lv_obj_t *part : {s_tile_kind, s_tile_place, s_tile_title}) {
        lv_obj_set_width(part, s_tile_w);
    }

    s_tile_after = theme::make_eyebrow(s_tile, "AFTER THAT");
    lv_obj_set_style_margin_top(s_tile_after, space::l, 0);
    const bool stacked = s_tile_w < TILE_SIDE_W;
    for (TileRow &row : s_tile_rows) {
        row.row = stacked ? column_of(s_tile, 0) : row_of(s_tile, theme::type_body()->line_height, 0);
        row.when = line_label(row.row, theme::secondary, theme::type_body());
        row.what = line_label(row.row, theme::text, theme::type_body());
        lv_obj_set_width(row.when, stacked ? s_tile_w : TILE_WHEN_W);
        lv_obj_set_width(row.what, stacked ? s_tile_w : s_tile_w - TILE_WHEN_W);
    }
    detail::subscribe(detail::Topic::Calendar, detail::kNoView, show_tile);
    detail::subscribe(detail::Topic::Page, detail::kNoView, show_tile);
    lv_timer_create([](lv_timer_t *) { show_tile(); }, REFRESH_MS, nullptr);
}

}  // namespace ui
