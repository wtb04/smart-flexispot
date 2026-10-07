// The Calendar page: the next event, with the way there or the rest of its
// day under it, or in the middle of its tile when neither is to be had, and
// the coming week as a list beside it; or the week as a grid in their place.
// A tap on any event shows it whole in a sheet.

#include "kit_front.h"

#include "ical.h"
#include "kit_fonts.h"
#include "topics.h"
#include "travel.h"
#include "units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;

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

constexpr int EVENTS_AHEAD = 40;  // read per refresh
constexpr int LIST_ITEMS   = 40;
constexpr int WEEK_EVENTS  = 48;
constexpr int WEEK_DAYS    = units::kDaysPerWeek;
constexpr int WORKDAYS     = 5;
constexpr int HOURS_MAX    = units::kHoursPerDay;
constexpr int ALSO_ITEMS   = 3;  // the rest of the next event's day, under it
constexpr int TITLE_LINES  = 2;

// The hours the week shows however little is on them.
constexpr float WORKDAY_FIRST_HOUR = 8.0f;
constexpr float WORKDAY_LAST_HOUR  = 18.0f;
// Over this many hours, a line an hour is a grid, so they go every other hour.
constexpr int HOURLY_MARKS_MAX = 12;

constexpr std::int64_t SHOW_WAYS_WITHIN = 3 * units::kSecondsPerHour;  // hours off is nothing to act on yet, beyond a line of when
constexpr std::int64_t LONG_EVENT       = 4 * units::kSecondsPerHour;  // said with its end, as a day at work

constexpr std::int32_t W = inset::width, H = inset::height;
constexpr std::int32_t LIST_W   = 480;
constexpr std::int32_t CARD_W   = W - LIST_W - space::l;
constexpr std::int32_t INNER_W  = CARD_W - 2 * space::xl;
constexpr std::int32_t HEAD_H   = size::button;
constexpr std::int32_t TIME_W   = 68;
constexpr std::int32_t MARK_W   = 4;
constexpr std::int32_t CHIP_H   = 44;
constexpr std::int32_t DOT      = 10;

constexpr std::int32_t HOUR_AXIS_W  = 44;
constexpr std::int32_t DAY_HEAD_H   = 30;
constexpr std::int32_t BLOCK_INSET  = 6;  // off the day's edges, so today's band shows round every event
constexpr std::int32_t BLOCK_APART  = 4;  // between events side by side
constexpr std::int32_t BLOCK_PAD    = 6;
constexpr std::int32_t BLOCK_RADIUS = 8;
constexpr std::int32_t NOW_LINE_H   = 2;
constexpr std::uint8_t BLOCK_MIX    = 110;
constexpr std::uint8_t PAST_MIX     = 40;
constexpr std::int32_t WORDS_MIN_W  = 64;  // narrower, a block is its colour and a tap shows the rest

constexpr std::int32_t SHEET_W     = 640;
constexpr std::int32_t SHEET_ROW_H = 52;
constexpr std::int32_t SHEET_BASE  = 116;  // its head, padding and the frame round the rows

// --- what an event says ---------------------------------------------------

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

std::tm local(std::int64_t at)
{
    std::tm    when{};
    const auto stamp = static_cast<std::time_t>(at);
    localtime_r(&stamp, &when);
    return when;
}

int whole_minutes(std::int64_t seconds) { return static_cast<int>(seconds / units::kSecondsPerMinute); }

int minutes_rounded_up(std::int64_t seconds)
{
    return static_cast<int>((seconds + units::kSecondsPerMinute - 1) / units::kSecondsPerMinute);
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
        std::snprintf(out, size, "in %dh %02dm", minutes / units::kMinutesPerHour, minutes % units::kMinutesPerHour);
    } else if (days == 1) {
        std::snprintf(out, size, "tomorrow");
    } else {
        std::snprintf(out, size, "in %d days", days);
    }
}

void day_name(std::int64_t now, std::int64_t at, char *out, std::size_t size)
{
    static const char *const WEEKDAYS[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
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

void date_of(std::int64_t at, char *out, std::size_t size)
{
    const std::tm when = local(at);
    char          date[16];
    std::strftime(date, sizeof(date), "%e %b", &when);
    std::snprintf(out, size, "%s", date[0] == ' ' ? date + 1 : date);
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

// A timetable's title is the course and the session, "Course - Session": each
// gets a line of its own rather than both being cut off.
void split_title(const char *summary, char *course, std::size_t course_size, char *session, std::size_t session_size)
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

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool is_upper(char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; }

// An address as street and town: "Voorbeeldstraat 12 1234AB Plaats Nederland" is
// "Voorbeeldstraat 12, Plaats".
void short_place(const char *place, char *out, std::size_t size)
{
    std::snprintf(out, size, "%s", place);
    for (char *at = out; at[0] != '\0'; ++at) {
        const bool postcode = (at == out || at[-1] == ' ') && is_digit(at[0]) && is_digit(at[1]) && is_digit(at[2]) &&
                              is_digit(at[3]) && is_upper(at[4]) && is_upper(at[5]);
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
        event_clock(event.end, until, sizeof(until));
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

void when_of(const ical::Event &event, char *out, std::size_t size)
{
    char from[16];
    char to[16];
    event_clock(event.start, from, sizeof(from));
    event_clock(event.end, to, sizeof(to));
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

// --- drawing helpers -------------------------------------------------------

void set_text_if(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

void fill_if(lv_obj_t *obj, lv_color_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), c)) {
        lv_obj_set_style_bg_color(obj, c, 0);
    }
}

lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    return obj;
}

const lv_font_t *font_of(lv_obj_t *label) { return lv_obj_get_style_text_font(label, LV_PART_MAIN); }

// One line, cut short with dots, at a size that never lays anything out again.
lv_obj_t *line(lv_obj_t *parent, Role role, Ink ink, std::int32_t width)
{
    lv_obj_t *l = text(parent, role, "", ink);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, width, font_of(l)->line_height);
    return l;
}

// Dots only shorten a label of fixed height, so a title allowed two lines is
// measured and given one or two.
std::int32_t fit_lines(lv_obj_t *label, std::int32_t width, int lines)
{
    const lv_font_t *font = font_of(label);
    lv_point_t       size{};
    lv_text_get_size(&size, lv_label_get_text(label), font, lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                     0, width, LV_TEXT_FLAG_NONE);
    const std::int32_t h = std::max<std::int32_t>(font->line_height, std::min<std::int32_t>(size.y, lines * font->line_height));
    lv_obj_set_height(label, h);
    return h;
}

lv_obj_t *feed_dot(lv_obj_t *parent)
{
    lv_obj_t *d = plain(parent);
    lv_obj_set_size(d, DOT, DOT);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    return d;
}

lv_obj_t *feed_mark(lv_obj_t *parent, std::int32_t h)
{
    lv_obj_t *m = plain(parent);
    lv_obj_set_size(m, MARK_W, h);
    lv_obj_set_style_radius(m, MARK_W / 2, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    return m;
}

// What a finger on a tappable part of a tile shows: as a pressed row.
lv_style_t s_pressed;

void restyle_pressed()
{
    lv_style_set_bg_color(&s_pressed, lv_color_hex(colours().raised_hi));
}

void pressable(lv_obj_t *obj, lv_event_cb_t clicked, void *data)
{
    lv_obj_set_clickable(obj, true);
    lv_obj_add_style(obj, &s_pressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(obj, clicked, LV_EVENT_CLICKED, data);
    popover_source(obj);
}

// --- the sheet -------------------------------------------------------------

void open_event(lv_obj_t *source, const ical::Event &event)
{
    if (popover_open()) {
        return;
    }
    const auto now = static_cast<std::int64_t>(detail::wall_now());
    char       course[ical::kSummaryMax];
    char       session[ical::kSummaryMax];
    split_title(event.summary, course, sizeof(course), session, sizeof(session));
    const bool placed = place_of(event)[0] != '\0';
    const int  rows   = 3 + (session[0] != '\0' ? 1 : 0) + (placed ? 1 : 0);

    // The title wraps in the sheet's head rather than pushing its close button off.
    const std::int32_t title_w = SHEET_W - 2 * space::xxl - size::button - space::m;
    lv_point_t         title{};
    lv_text_get_size(&title, course, fonts::h2(), 0, 0, title_w, LV_TEXT_FLAG_NONE);
    const std::int32_t extra = std::max<std::int32_t>(0, title.y - fonts::h2()->line_height);
    lv_obj_t *body = sheet(source, course, SHEET_W, SHEET_BASE + rows * SHEET_ROW_H + extra);
    lv_obj_t *head = lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(body)), 0);
    lv_obj_t *name = lv_obj_get_child(head, 0);
    lv_obj_set_width(name, title_w);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_WRAP);

    char text[ical::kSummaryMax + 32];
    sheet_row(body, "What", feed_kind(event.feed));
    char day[24];
    char date[16];
    day_name(now, event.start, day, sizeof(day));
    date_of(event.start, date, sizeof(date));
    if (std::strchr(day, ' ') != nullptr) {
        std::snprintf(text, sizeof(text), "%s", day);  // its date in it already
    } else {
        std::snprintf(text, sizeof(text), "%s, %s", day, date);
    }
    sheet_row(body, "Day", text);
    when_of(event, text, sizeof(text));
    sheet_row(body, "Time", text);
    if (session[0] != '\0') {
        sheet_row(body, "Session", session);
    }
    if (placed) {
        sheet_row(body, "Where", place_of(event));
    }
}

// --- the way there -----------------------------------------------------------

bool is_mode(const char *mode, const char *name) { return std::strcmp(mode, name) == 0; }
bool is_walk_or_bike(const char *mode) { return is_mode(mode, "walk") || is_mode(mode, "bike"); }

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

// Where the walk at the end goes, which the backend leaves to be named: the
// room the event is in, or just there.
const char *end_name(const char *place) { return place != nullptr && place[0] != '\0' ? place : "There"; }

const char *glyph_of(const char *mode)
{
    if (is_mode(mode, "walk")) {
        return ic::directions_walk;
    }
    if (is_mode(mode, "bike")) {
        return ic::directions_bike;
    }
    return is_mode(mode, "train") ? ic::train : ic::directions_bus;
}

// The ways there still worth taking, latest first as the backend answers, and
// the one on show: the best, which is the first that is not late, unless
// another was picked by hand. A pick is held by when it leaves, so it stays
// through refetches and falls away once gone. The choices run earliest first.
KIT_IN_PSRAM travel::Option s_ways[travel::kOptionsMax];
int                         s_way_count = 0;
int                         s_way_order[travel::kOptionsMax];
std::int64_t                s_picked_leave = 0;
const travel::Option       *s_going        = nullptr;
const travel::Option       *s_later        = nullptr;  // the best way while they are hours off

void keep_ways_worth_taking(bool wanted, std::int64_t now)
{
    static KIT_IN_PSRAM travel::Option options[travel::kOptionsMax];
    const int found = wanted ? travel::options(options, travel::kOptionsMax) : 0;
    s_way_count     = 0;
    for (int i = 0; i < found; ++i) {
        if (options[i].leave >= now) {
            s_ways[s_way_count++] = options[i];
        }
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
    s_later = nullptr;
    if (s_way_count > 0 && s_ways[0].leave - now > SHOW_WAYS_WITHIN) {
        s_later     = best_way();
        s_way_count = 0;
    }
    s_going = picked_way();
    if (s_going == nullptr) {
        s_picked_leave = 0;
        s_going        = best_way();
    }
    order_ways_by_leaving();
}

// What to do at a step: the train and where it goes, or where to walk or ride to.
void step_text(const travel::Leg &leg, bool last, const char *place, char *what, std::size_t what_size, char *detail,
               std::size_t detail_size)
{
    const int   mins  = minutes_rounded_up(leg.arrive - leg.depart);
    const char *about = leg.estimated ? "about " : "";
    const char *to    = last ? end_name(place) : leg.to;
    char        out_at[16];
    event_clock(leg.arrive, out_at, sizeof(out_at));
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

// --- the overview ------------------------------------------------------------

struct Step {  // one thing to do on the way there
    lv_obj_t *root = nullptr, *time = nullptr, *glyph = nullptr, *what = nullptr, *detail = nullptr;
};

struct Also {  // one more event on the next one's day
    lv_obj_t *root = nullptr, *time = nullptr, *mark = nullptr, *title = nullptr, *note = nullptr;
};

struct Slot {  // a day's name, or an event, in the coming week's list
    lv_obj_t *day = nullptr, *row = nullptr;
};

lv_obj_t *s_overview = nullptr, *s_week = nullptr;
bool      s_detailed = false;

lv_obj_t *s_card = nullptr, *s_top = nullptr, *s_none = nullptr, *s_card_lead = nullptr, *s_card_gap = nullptr;
lv_obj_t *s_kind_dot = nullptr, *s_kind = nullptr, *s_title = nullptr, *s_session = nullptr;
lv_obj_t *s_start = nullptr, *s_until = nullptr, *s_place = nullptr;
lv_obj_t *s_later_row = nullptr, *s_later_glyph = nullptr, *s_later_text = nullptr;

lv_obj_t *s_foot = nullptr, *s_foot_head = nullptr;
lv_obj_t *s_leave_row = nullptr, *s_leave = nullptr, *s_there = nullptr, *s_spare = nullptr;
Step      s_step[travel::kLegsMax];
lv_obj_t *s_choices = nullptr;
lv_obj_t *s_choice[travel::kOptionsMax];
Also      s_also[ALSO_ITEMS];

lv_obj_t *s_list = nullptr, *s_nothing = nullptr;
Slot      s_slot[LIST_ITEMS];

KIT_IN_PSRAM ical::Event s_next_event;
KIT_IN_PSRAM ical::Event s_also_event[ALSO_ITEMS];
KIT_IN_PSRAM ical::Event s_slot_event[LIST_ITEMS];
KIT_IN_PSRAM ical::Event s_week_event[WEEK_EVENTS];

void show_calendar();

void on_event_tap(lv_event_t *e)
{
    open_event(lv_event_get_current_target_obj(e), *static_cast<const ical::Event *>(lv_event_get_user_data(e)));
}

// A step on the way or another event of the day: a line and a quieter one under it.
std::int32_t two_lines() { return fonts::body()->line_height + fonts::small()->line_height; }

// Hours before it, when to go, quietly: the way itself once it is near.
void show_later(std::int64_t starts)
{
    lv_obj_set_hidden(s_later_row, s_later == nullptr);
    if (s_later == nullptr) {
        return;
    }
    char text[96];
    char leave[16];
    char there[16];
    event_clock(s_later->leave, leave, sizeof(leave));
    event_clock(s_later->arrive, there, sizeof(there));
    if (s_later->cancelled) {
        std::snprintf(text, sizeof(text), "Leave at %s, cancelled", leave);
    } else {
        const int spare = whole_minutes(starts - s_later->arrive);
        std::snprintf(text, sizeof(text), "Leave at %s, there %s, %d min %s", leave, there, spare >= 0 ? spare : -spare,
                      spare >= 0 ? "early" : "late");
    }
    set_text_if(s_later_text, text);
    set_icon(s_later_glyph, glyph_of(s_later->leg_count > 0 ? s_later->legs[0].mode : "walk"), false);
}

void show_next(const ical::Event *first, std::int64_t now)
{
    const bool way = first != nullptr && s_going != nullptr;
    lv_obj_set_hidden(s_top, first == nullptr);
    lv_obj_set_hidden(s_none, first != nullptr);
    lv_obj_set_hidden(s_card_gap, first == nullptr);  // the empty card's line in its middle
    lv_obj_set_hidden(s_card_lead, first == nullptr || way);  // in the middle, with nothing to do under it
    if (first == nullptr) {
        return;
    }
    s_next_event = *first;
    char text[ical::kSummaryMax + 32];
    fill_if(s_kind_dot, lv_color_hex(event_ink(first->feed)));
    event_kind(*first, now, text, sizeof(text));
    set_text_if(s_kind, text);

    char session[ical::kSummaryMax];
    split_title(first->summary, text, sizeof(text), session, sizeof(session));
    set_text_if(s_title, text);
    fit_lines(s_title, INNER_W, way ? 1 : TITLE_LINES);
    set_text_if(s_session, session);
    lv_obj_set_hidden(s_session, session[0] == '\0');

    event_clock(first->start, text, sizeof(text));
    set_text_if(s_start, text);
    char to[24];
    event_clock(first->end, text, sizeof(text));
    std::snprintf(to, sizeof(to), "\xe2\x80\x93 %.16s", text);
    set_text_if(s_until, to);
    event_place(*first, text, sizeof(text));
    set_text_if(s_place, text);
    // a way there that ends on foot or by bike names the place as its last step
    const bool named = way && s_going->leg_count > 0 && is_walk_or_bike(s_going->legs[s_going->leg_count - 1].mode);
    lv_obj_set_hidden(s_place, text[0] == '\0' || named);
    show_later(first->start);
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
        char                  text[40];
        event_clock(way.leave, text, sizeof(text));
        const std::size_t at = std::strlen(text);
        if (way.cancelled) {
            std::snprintf(text + at, sizeof(text) - at, ", cancelled");
        } else if (way.late && starts > 0) {
            std::snprintf(text + at, sizeof(text) - at, ", %d min late", minutes_rounded_up(way.arrive - starts));
        }
        set_text_if(lv_obj_get_child(s_choice[i], 0), text);
        lv_obj_set_state(s_choice[i], LV_STATE_CHECKED, &way == s_going);
    }
}

// The way there as what to do: when to leave, then each step.
void show_journey(std::int64_t starts, const char *place)
{
    char text[64];
    set_text_if(s_foot_head, s_going->cancelled ? "Leave at, cancelled" : "Leave at");
    ink_if(s_foot_head, s_going->cancelled ? Ink::Red : Ink::Secondary);
    event_clock(s_going->leave, text, sizeof(text));
    set_text_if(s_leave, text);
    ink_if(s_leave, s_going->cancelled ? Ink::Red : Ink::Primary);
    char there[sizeof(text) + 8];
    event_clock(s_going->arrive, text, sizeof(text));
    std::snprintf(there, sizeof(there), "there %s", text);
    set_text_if(s_there, there);
    const int spare = whole_minutes(starts - s_going->arrive);
    std::snprintf(text, sizeof(text), "%d min %s", spare >= 0 ? spare : -spare, spare >= 0 ? "early" : "late");
    set_text_if(s_spare, text);
    ink_if(s_spare, spare >= 0 ? Ink::Green : Ink::Amber);

    for (int i = 0; i < travel::kLegsMax; ++i) {
        Step      &step = s_step[i];
        const bool real = i < s_going->leg_count;
        lv_obj_set_hidden(step.root, !real);
        if (!real) {
            continue;
        }
        const travel::Leg &leg = s_going->legs[i];
        event_clock(leg.depart, text, sizeof(text));
        set_text_if(step.time, text);
        ink_if(step.time, leg.cancelled ? Ink::Red : leg.estimated ? Ink::Secondary : Ink::Primary);
        set_icon(step.glyph, glyph_of(leg.mode), false);
        ink_if(step.glyph, leg.cancelled ? Ink::Red : Ink::Secondary);
        char what[96];
        char detail[96];
        step_text(leg, i == s_going->leg_count - 1, place, what, sizeof(what), detail, sizeof(detail));
        set_text_if(step.what, what);
        set_text_if(step.detail, detail);
        ink_if(step.detail, leg.cancelled ? Ink::Red : Ink::Secondary);
    }
    show_choices(starts);
}

// What else is on the next event's day, under it, a clash said in words.
// Returns how many of those after it are shown.
int show_also(const ical::Event *ahead, int count, std::int64_t now)
{
    const ical::Event &next  = ahead[0];
    const int          day   = days_from(now, next.start);
    int                shown = 0;
    for (int i = 1; i < count && shown < ALSO_ITEMS; ++i) {
        if (days_from(now, ahead[i].start) != day) {
            break;
        }
        Also &also          = s_also[shown];
        s_also_event[shown] = ahead[i];
        char text[ical::kSummaryMax + 32];
        event_clock(ahead[i].start, text, sizeof(text));
        set_text_if(also.time, text);
        fill_if(also.mark, lv_color_hex(event_ink(ahead[i].feed)));
        list_title(ahead[i], text, sizeof(text));
        set_text_if(also.title, text);
        char place[64];
        char note[96];
        short_place(place_of(ahead[i]), place, sizeof(place));
        std::snprintf(note, sizeof(note), "%s%s%s%s", feed_kind(ahead[i].feed), place[0] != '\0' ? ", " : "", place,
                      ahead[i].start < next.end ? ", at the same time" : "");
        set_text_if(also.note, note);
        lv_obj_set_hidden(also.root, false);
        ++shown;
    }
    for (int i = shown; i < ALSO_ITEMS; ++i) {
        lv_obj_set_hidden(s_also[i].root, true);
    }
    char name[24];
    day_name(now, next.start, name, sizeof(name));
    char head[40];
    std::snprintf(head, sizeof(head), "Also %s%s", day <= 1 ? "" : "on ", name);
    for (char *c = head + 5; *c != '\0' && day <= 1; ++c) {
        *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));  // "Also today", "Also on Monday"
    }
    set_text_if(s_foot_head, head);
    ink_if(s_foot_head, Ink::Secondary);
    return shown;
}

// Under the next event: the way there while there is one, else the rest of its day.
int show_foot(const ical::Event *ahead, int count, std::int64_t now)
{
    const bool way = s_going != nullptr && count > 0;
    lv_obj_set_hidden(s_leave_row, !way);
    if (!way) {
        for (Step &step : s_step) {
            lv_obj_set_hidden(step.root, true);
        }
        lv_obj_set_hidden(s_choices, true);
    }
    int also = 0;
    if (way) {
        for (Also &a : s_also) {
            lv_obj_set_hidden(a.root, true);
        }
        show_journey(ahead[0].start, place_of(ahead[0]));
        if (s_going->leg_count > 3) {
            lv_obj_set_hidden(s_session, true);  // a long route takes the session's line
        }
        lv_obj_set_hidden(s_foot, false);
    } else if (count > 0) {
        also = show_also(ahead, count, now);
        lv_obj_set_hidden(s_foot, also == 0);
    } else {
        lv_obj_set_hidden(s_foot, true);
    }
    return also;
}

// The coming seven days, rather than what is left of the calendar week, which
// on a Friday is nothing; what is under the next event already is left out.
void show_list(const ical::Event *ahead, int count, int skip, std::int64_t now)
{
    const std::int64_t week_end = now + SECONDS_PER_WEEK;
    int                used     = 0;
    int                last_day = -1000;
    for (int i = 1 + skip; i < count && used < LIST_ITEMS && ahead[i].start < week_end; ++i) {
        Slot     &slot = s_slot[used];
        const int day  = days_from(now, ahead[i].start);
        char      text[ical::kSummaryMax + 96];
        const bool new_day = day != last_day;
        lv_obj_set_hidden(slot.day, !new_day);
        if (new_day) {
            last_day = day;
            day_name(now, ahead[i].start, text, sizeof(text));
            set_text_if(lv_obj_get_child(slot.day, 0), text);
            if (std::strchr(text, ' ') != nullptr) {
                text[0] = '\0';  // a date in its name already
            } else {
                date_of(ahead[i].start, text, sizeof(text));
            }
            set_text_if(lv_obj_get_child(slot.day, 1), text);
        }
        s_slot_event[used] = ahead[i];
        event_clock(ahead[i].start, text, sizeof(text));
        set_row_lead(slot.row, text, Ink::Primary);
        set_row_mark(slot.row, event_ink(ahead[i].feed));
        list_title(ahead[i], text, sizeof(text));
        set_row_title(slot.row, text);
        list_place(ahead[i], text, sizeof(text));
        set_row_detail(slot.row, text);
        lv_obj_set_hidden(slot.row, false);
        ++used;
    }
    for (int i = used; i < LIST_ITEMS; ++i) {
        lv_obj_set_hidden(s_slot[i].day, true);
        lv_obj_set_hidden(s_slot[i].row, true);
    }
    lv_obj_set_hidden(s_nothing, used > 0);
}

void choice_clicked(lv_event_t *e)
{
    const auto index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (index < s_way_count) {
        s_picked_leave = s_ways[s_way_order[index]].leave;
        show_calendar();
    }
}

void build_top(lv_obj_t *card)
{
    s_top = flow(card, LV_FLEX_FLOW_COLUMN, space::xs);
    lv_obj_set_width(s_top, INNER_W + 2 * space::s);
    lv_obj_set_style_radius(s_top, radius::inner, 0);
    lv_obj_set_style_pad_all(s_top, space::s, 0);
    lv_obj_set_style_margin_hor(s_top, -space::s, 0);
    lv_obj_set_style_margin_top(s_top, -space::s, 0);
    pressable(s_top, on_event_tap, &s_next_event);

    lv_obj_t *kind = flow(s_top, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_flex_align(kind, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_kind_dot = feed_dot(kind);
    s_kind     = line(kind, Role::Small, Ink::Secondary, INNER_W - DOT - space::s);

    s_title = text(s_top, Role::H1, "");
    lv_obj_set_width(s_title, INNER_W);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    s_session = line(s_top, Role::Body, Ink::Secondary, INNER_W);

    // When it starts, large, and when it ends beside it on the figures' baseline.
    const lv_font_t *big   = fonts::read_l();
    const lv_font_t *small = fonts::h2();
    lv_obj_t        *when  = plain(s_top);
    lv_obj_set_size(when, INNER_W, big->line_height);
    lv_obj_set_style_margin_top(when, space::s, 0);
    const std::int32_t start_w = widest(Role::ReadL, {"88:88"});
    s_start                    = line(when, Role::ReadL, Ink::Primary, start_w);
    s_until                    = line(when, Role::H2, Ink::Secondary, widest(Role::H2, {"\xe2\x80\x93 88:88"}));
    lv_obj_set_pos(s_until, start_w + space::m,
                   (big->line_height - big->base_line) - (small->line_height - small->base_line));
    s_place = line(s_top, Role::H2, Ink::Primary, INNER_W);

    s_later_row = flow(s_top, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_flex_align(s_later_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(s_later_row, space::s, 0);
    s_later_glyph = icon(s_later_row, ic::directions_walk, 24, false, Ink::Secondary);
    s_later_text  = line(s_later_row, Role::Body, Ink::Secondary, INNER_W - 24 - space::s);
    lv_obj_set_hidden(s_later_row, true);

    s_none = empty(card, ic::event, "Nothing coming up");
    lv_obj_set_hidden(s_none, true);
}

void build_step(Step &step)
{
    step.root = plain(s_foot);
    lv_obj_set_size(step.root, INNER_W, two_lines());
    step.time  = line(step.root, Role::Body, Ink::Primary, TIME_W);
    step.glyph = icon(step.root, ic::directions_walk, 24, false, Ink::Secondary);
    lv_obj_set_pos(step.glyph, TIME_W, 0);
    step.what   = line(step.root, Role::Body, Ink::Primary, INNER_W - TIME_W - 36);
    step.detail = line(step.root, Role::Small, Ink::Secondary, INNER_W - TIME_W - 36);
    lv_obj_set_pos(step.what, TIME_W + 36, 0);
    lv_obj_set_pos(step.detail, TIME_W + 36, fonts::body()->line_height);
    lv_obj_set_hidden(step.root, true);
}

void build_also(Also &also, int index)
{
    also.root = plain(s_foot);
    lv_obj_set_size(also.root, INNER_W + 2 * space::s, two_lines() + 2 * space::xs);
    lv_obj_set_style_margin_hor(also.root, -space::s, 0);
    lv_obj_set_style_radius(also.root, radius::inner, 0);
    lv_obj_set_style_pad_hor(also.root, space::s, 0);
    lv_obj_set_style_pad_top(also.root, space::xs, 0);
    pressable(also.root, on_event_tap, &s_also_event[index]);
    also.time = line(also.root, Role::Body, Ink::Primary, TIME_W);
    also.mark = feed_mark(also.root, two_lines() - 2 * space::xs);
    lv_obj_set_pos(also.mark, TIME_W, space::xs);
    also.title = line(also.root, Role::Body, Ink::Primary, INNER_W - TIME_W - space::l);
    also.note  = line(also.root, Role::Small, Ink::Secondary, INNER_W - TIME_W - space::l);
    lv_obj_set_pos(also.title, TIME_W + space::l, 0);
    lv_obj_set_pos(also.note, TIME_W + space::l, fonts::body()->line_height);
    lv_obj_set_hidden(also.root, true);
}

// Under the next event, set off by a line: the way there, or the rest of its day.
void build_foot(lv_obj_t *card)
{
    s_foot = flow(card, LV_FLEX_FLOW_COLUMN, space::xs);
    lv_obj_set_width(s_foot, LV_PCT(100));
    hairline(s_foot, false, INNER_W);
    s_foot_head = line(s_foot, Role::Small, Ink::Secondary, INNER_W);

    const lv_font_t *big = fonts::read_l();
    s_leave_row          = plain(s_foot);
    s_leave                  = line(s_leave_row, Role::ReadL, Ink::Primary, widest(Role::ReadL, {"88:88"}));
    const std::int32_t there = widest(Role::H3, {"there 88:88"});
    const std::int32_t spare = widest(Role::Small, {"888 min early"});
    s_there                  = line(s_leave_row, Role::H3, Ink::Primary, there);
    s_spare                  = line(s_leave_row, Role::Small, Ink::Green, spare);
    for (lv_obj_t *l : {s_there, s_spare}) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
    }
    // The two lines at the right stand on the figures' baseline, the row tall enough for both.
    const lv_font_t   *sm      = fonts::small();
    const std::int32_t spare_y = (big->line_height - big->base_line) - (sm->line_height - sm->base_line);
    const std::int32_t there_y = spare_y - fonts::h3()->line_height;
    const std::int32_t down    = std::max<std::int32_t>(0, -there_y);
    lv_obj_set_pos(s_leave, 0, down);
    lv_obj_set_pos(s_spare, INNER_W - spare, spare_y + down);
    lv_obj_set_pos(s_there, INNER_W - there, there_y + down);
    lv_obj_set_size(s_leave_row, INNER_W, std::max(big->line_height, spare_y + sm->line_height) + down);

    for (Step &step : s_step) {
        build_step(step);
    }
    s_choices = flow(s_foot, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_flex_align(s_choices, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(s_choices, Role::Small, "Or leave at", Ink::Secondary);
    for (int i = 0; i < travel::kOptionsMax; ++i) {
        s_choice[i] = button(s_choices, Button::Raised, nullptr, "", false, true);
        lv_obj_set_height(s_choice[i], CHIP_H);
        lv_obj_set_style_radius(s_choice[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_add_event_cb(s_choice[i], choice_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
    }
    lv_obj_set_hidden(s_choices, true);
    for (int i = 0; i < ALSO_ITEMS; ++i) {
        build_also(s_also[i], i);
    }
}

void build_card(lv_obj_t *parent)
{
    s_card = tile(parent, Lift::Low);
    lv_obj_set_size(s_card, CARD_W, H);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_card, space::s, 0);
    s_card_lead = filler(s_card);
    build_top(s_card);
    s_card_gap = filler(s_card);
    build_foot(s_card);
}

void toggle_clicked(lv_event_t *);

// The coming week as a list that scrolls inside its tile, a button at its
// head for the week as a grid.
void build_coming_up(lv_obj_t *parent)
{
    s_list        = list(parent, LIST_W, H);
    lv_obj_t *box = lv_obj_get_parent(s_list);
    const std::int32_t pad = lv_obj_get_style_pad_top(box, LV_PART_MAIN);
    lv_obj_t *head = flow(box, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_size(head, LV_PCT(100), HEAD_H);
    lv_obj_set_style_pad_left(head, space::m, 0);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(head, Role::H3, "Coming up");
    filler(head);
    lv_obj_t *week = button(head, Button::Raised, ic::calendar_month, "Week");
    lv_obj_add_event_cb(week, toggle_clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_pos(s_list, 0, HEAD_H + space::xs);
    lv_obj_set_height(s_list, H - 2 * pad - HEAD_H - space::xs);

    RowSpec spec;
    spec.lead      = "";
    spec.lead_w    = widest(Role::Body, {"88:88"});
    spec.mark      = INK_OTHER;
    spec.title     = "";
    spec.detail    = "";
    spec.pressable = true;
    for (int i = 0; i < LIST_ITEMS; ++i) {
        Slot &slot = s_slot[i];
        slot.day   = section(s_list, "", "");
        slot.row   = row(s_list, spec);
        lv_obj_add_event_cb(slot.row, on_event_tap, LV_EVENT_CLICKED, &s_slot_event[i]);
        popover_source(slot.row);
        lv_obj_set_hidden(slot.day, true);
        lv_obj_set_hidden(slot.row, true);
    }
    s_nothing = empty(s_list, ic::event, "Nothing else in the coming week");
    lv_obj_set_hidden(s_nothing, true);
}

// --- the week ------------------------------------------------------------------

struct Block {  // one event in the week
    lv_obj_t *root = nullptr, *time = nullptr, *title = nullptr, *place = nullptr;
};

lv_obj_t    *s_grid = nullptr;
lv_obj_t    *s_week_name = nullptr, *s_week_no = nullptr, *s_week_back = nullptr;
lv_obj_t    *s_band = nullptr, *s_now = nullptr;
lv_obj_t    *s_day_head[WEEK_DAYS];
lv_obj_t    *s_hour_mark[HOURS_MAX + 1];
lv_obj_t    *s_hour_line[HOURS_MAX + 1];
Block        s_block[WEEK_EVENTS];
int          s_week_shift = 0;  // weeks away from the one worth looking at
std::int32_t s_grid_w = 0, s_area_h = 0;
constexpr std::int32_t AREA_Y = DAY_HEAD_H + space::s;  // the hours, under the days' names

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
    return static_cast<float>(when.tm_hour) + static_cast<float>(when.tm_min) / static_cast<float>(units::kMinutesPerHour);
}

struct WeekGrid {
    std::int64_t from;
    int          days;
    float        first, last;
    int          hours;
    float        px_per_hour;
    std::int32_t col_w;
    int          today;  // off the grid on another week
};

WeekGrid grid_for(std::int64_t from, std::int64_t now, const ical::Event *week, int count, int *day_of)
{
    WeekGrid grid{};
    grid.from  = from;
    grid.days  = WORKDAYS;  // the weekend only when something is on it
    grid.first = WORKDAY_FIRST_HOUR;
    grid.last  = WORKDAY_LAST_HOUR;
    for (int i = 0; i < count; ++i) {
        day_of[i]           = std::clamp(days_from(from, week[i].start), 0, WEEK_DAYS - 1);
        grid.days           = std::max(grid.days, day_of[i] + 1);
        grid.first          = std::min(grid.first, std::floor(hour_of(std::max(week[i].start, from))));
        const bool same_day = days_from(week[i].start, week[i].end) == 0;
        grid.last = std::max(grid.last, same_day ? std::ceil(hour_of(week[i].end)) : static_cast<float>(HOURS_MAX));
    }
    grid.hours       = static_cast<int>(grid.last - grid.first);
    grid.px_per_hour = static_cast<float>(s_area_h) / static_cast<float>(grid.hours);
    grid.col_w       = (s_grid_w - HOUR_AXIS_W) / grid.days;
    grid.today       = days_from(from, now);
    return grid;
}

std::int32_t column_x(const WeekGrid &grid, int day) { return HOUR_AXIS_W + day * grid.col_w; }

void show_hour_lines(const WeekGrid &grid)
{
    const std::int32_t line_h = fonts::small()->line_height;
    const int          every  = grid.hours > HOURLY_MARKS_MAX ? 2 : 1;
    for (int h = 0; h <= HOURS_MAX; ++h) {
        const bool real = h <= grid.hours && h % every == 0;
        lv_obj_set_hidden(s_hour_mark[h], !real || h == grid.hours);
        lv_obj_set_hidden(s_hour_line[h], !real);
        if (!real) {
            continue;
        }
        const auto y = AREA_Y + static_cast<std::int32_t>(static_cast<float>(h) * grid.px_per_hour);
        char       text[8];
        std::snprintf(text, sizeof(text), "%02d", (static_cast<int>(grid.first) + h) % HOURS_MAX);
        set_text_if(s_hour_mark[h], text);
        lv_obj_set_pos(s_hour_mark[h], 0, std::max<std::int32_t>(AREA_Y, y - line_h / 2));
        lv_obj_set_pos(s_hour_line[h], HOUR_AXIS_W, y);
    }
}

void show_day_heads(const WeekGrid &grid)
{
    static const char *const NAMES[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    for (int d = 0; d < WEEK_DAYS; ++d) {
        lv_obj_set_hidden(s_day_head[d], d >= grid.days);
        if (d >= grid.days) {
            continue;
        }
        const std::tm date = noon_on(grid.from, d);
        char          text[16];
        std::snprintf(text, sizeof(text), "%s %d", NAMES[d], date.tm_mday);
        set_text_if(s_day_head[d], text);
        ink_if(s_day_head[d], d == grid.today ? Ink::Accent : Ink::Secondary);
        lv_obj_set_width(s_day_head[d], grid.col_w);
        lv_obj_set_pos(s_day_head[d], column_x(grid, d), (DAY_HEAD_H - fonts::h3()->line_height) / 2);
    }
    const bool today = grid.today >= 0 && grid.today < grid.days;
    lv_obj_set_hidden(s_band, !today);
    if (today) {
        lv_obj_set_pos(s_band, column_x(grid, grid.today), 0);
        lv_obj_set_size(s_band, grid.col_w, AREA_Y + s_area_h);
    }
}

// Each event takes the first lane free at its start; a run of overlaps is as
// narrow as its widest point.
void assign_lanes(const ical::Event *week, int count, const int *day_of, int days, int *lane, int *lanes_across)
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

void show_block(const Block &block, const ical::Event &event, const WeekGrid &grid, int day, int lane, int lanes,
                std::int64_t now)
{
    const std::int32_t line_h = fonts::small()->line_height;
    const std::int32_t lane_w = (grid.col_w - 2 * BLOCK_INSET + BLOCK_APART) / lanes;
    const float        top_h  = std::max(hour_of(std::max(event.start, grid.from)), grid.first) - grid.first;
    const float        end_h  = days_from(event.start, event.end) == 0 ? hour_of(event.end) : grid.last;
    const auto         y      = static_cast<std::int32_t>(top_h * grid.px_per_hour);
    const std::int32_t h      = std::max<std::int32_t>(
        static_cast<std::int32_t>((end_h - grid.first) * grid.px_per_hour) - y - 2, line_h + BLOCK_PAD);
    const std::int32_t w = lane_w - BLOCK_APART;
    lv_obj_set_pos(block.root, column_x(grid, day) + BLOCK_INSET + lane * lane_w, AREA_Y + y + 1);
    lv_obj_set_size(block.root, w, h);

    // Faded by colour rather than by opacity, which would show the lines
    // behind through a past event.
    const lv_color_t ink  = lv_color_hex(event_ink(event.feed));
    const lv_color_t card = lv_color_hex(colours().surface);
    const bool       over = event.end < now;
    fill_if(block.root, lv_color_mix(ink, card, over ? PAST_MIX : BLOCK_MIX));
    const lv_color_t edge = over ? lv_color_mix(ink, card, BLOCK_MIX) : ink;
    if (!lv_color_eq(lv_obj_get_style_border_color(block.root, LV_PART_MAIN), edge)) {
        lv_obj_set_style_border_color(block.root, edge, 0);
    }

    // The start, and the title where there is width for words: a sliver shows
    // only its colour, and a tap shows the rest.
    const std::int32_t inner = w - BLOCK_PAD - 3 - BLOCK_PAD;
    const bool         words = inner >= WORDS_MIN_W;
    const Ink          text_ink = over ? Ink::Secondary : Ink::Primary;
    char               text[ical::kSummaryMax];
    event_clock(event.start, text, sizeof(text));
    set_text_if(block.time, text);
    ink_if(block.time, text_ink);
    lv_obj_set_hidden(block.time, !words);
    event_course(event, text, sizeof(text));
    set_text_if(block.title, text);
    ink_if(block.title, text_ink);
    const std::int32_t spare  = std::max<std::int32_t>(1, (h - BLOCK_PAD - line_h) / line_h);
    char               place[64];
    short_place(place_of(event), place, sizeof(place));
    const bool         placed = words && place[0] != '\0' && spare >= 2;
    const std::int32_t lines  = placed ? std::min<std::int32_t>(spare - 1, TITLE_LINES) : std::min<std::int32_t>(spare, 3);
    lv_obj_set_hidden(block.title, !words || h < 2 * line_h);
    lv_obj_set_width(block.title, inner);
    const std::int32_t title_h = fit_lines(block.title, inner, lines);
    set_text_if(block.place, place);
    lv_obj_set_width(block.place, inner);
    lv_obj_set_pos(block.place, 0, line_h + title_h);
    lv_obj_set_hidden(block.place, !placed);
    lv_obj_set_hidden(block.root, false);
}

void show_now_line(const WeekGrid &grid, std::int64_t now)
{
    const float hour  = hour_of(now);
    const bool  shown = grid.today >= 0 && grid.today < grid.days && hour >= grid.first && hour <= grid.last;
    lv_obj_set_hidden(s_now, !shown);
    fill_if(s_now, colour(Ink::Accent));
    if (shown) {
        lv_obj_set_pos(s_now, column_x(grid, grid.today),
                       AREA_Y + static_cast<std::int32_t>((hour - grid.first) * grid.px_per_hour) - NOW_LINE_H / 2);
        lv_obj_set_width(s_now, grid.col_w);
    }
}

void show_week_name(const WeekGrid &grid)
{
    const std::tm monday = noon_on(grid.from, 0);
    const std::tm last   = noon_on(grid.from, grid.days - 1);
    char          week_no[8];
    std::strftime(week_no, sizeof(week_no), "%V", &monday);
    char first_month[8];
    char last_month[8];
    std::strftime(first_month, sizeof(first_month), "%b", &monday);
    std::strftime(last_month, sizeof(last_month), "%b", &last);
    char text[48];
    std::snprintf(text, sizeof(text), "Week %d", std::atoi(week_no));
    set_text_if(s_week_no, text);
    if (monday.tm_mon == last.tm_mon) {
        std::snprintf(text, sizeof(text), "%d \xe2\x80\x93 %d %s", monday.tm_mday, last.tm_mday, last_month);
    } else {
        std::snprintf(text, sizeof(text), "%d %s \xe2\x80\x93 %d %s", monday.tm_mday, first_month, last.tm_mday,
                      last_month);
    }
    set_text_if(s_week_name, text);
}

void show_week(std::int64_t now)
{
    static KIT_IN_PSRAM ical::Event week[WEEK_EVENTS];
    const std::int64_t from = week_start(now, s_week_shift);
    // An hour over, for the week summer time ends.
    const int count = ical::between(from, from + SECONDS_PER_WEEK + units::kSecondsPerHour, week, WEEK_EVENTS);

    int            day_of[WEEK_EVENTS];
    const WeekGrid grid = grid_for(from, now, week, count, day_of);
    show_hour_lines(grid);
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
        show_block(s_block[i], week[i], grid, day_of[i], lane[i], lanes_across[i], now);
    }
    show_now_line(grid, now);
    show_week_name(grid);
    lv_obj_set_hidden(s_week_back, s_week_shift == 0);
}

void week_step_clicked(lv_event_t *e)
{
    s_week_shift += static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    show_calendar();
}

void build_week_head(lv_obj_t *week)
{
    lv_obj_t *head = flow(week, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_size(head, LV_PCT(100), HEAD_H);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int step : {-1, 1}) {
        lv_obj_t *b = button(head, Button::Raised, step < 0 ? ic::chevron_left : ic::chevron_right, nullptr, true);
        lv_obj_set_size(b, size::button, size::button);
        lv_obj_add_event_cb(b, week_step_clicked, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(step)));
        if (step < 0) {
            s_week_name = line(head, Role::H3, Ink::Primary, widest(Role::H3, {"28 Sep \xe2\x80\x93 30 Oct"}) + space::s);
            lv_obj_set_style_text_align(s_week_name, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    s_week_back = button(head, Button::Raised, nullptr, "This week");
    lv_obj_add_event_cb(
        s_week_back,
        [](lv_event_t *) {
            s_week_shift = 0;
            show_calendar();
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_week_back, true);
    filler(head);
    s_week_no = line(head, Role::Small, Ink::Secondary, widest(Role::Small, {"Week 88"}));
    lv_obj_set_style_text_align(s_week_no, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *close = button(head, Button::Raised, ic::close, nullptr, true);
    lv_obj_set_size(close, size::button, size::button);
    lv_obj_add_event_cb(close, toggle_clicked, LV_EVENT_CLICKED, nullptr);
}

void build_week(lv_obj_t *page)
{
    s_week = tile(page, Lift::Low);
    lv_obj_set_size(s_week, W, H);
    build_week_head(s_week);

    const std::int32_t pad = lv_obj_get_style_pad_top(s_week, LV_PART_MAIN);
    s_grid_w               = W - 2 * pad;
    s_area_h               = H - 2 * pad - HEAD_H - space::l - AREA_Y - space::xs;
    s_grid                 = plain(s_week);
    lv_obj_set_pos(s_grid, 0, HEAD_H + space::l);
    lv_obj_set_size(s_grid, s_grid_w, AREA_Y + s_area_h + space::xs);

    // Only today's column has a ground, so the week reads as its events rather
    // than as a grid of columns.
    s_band = well(s_grid);
    lv_obj_set_style_pad_all(s_band, 0, 0);
    for (int h = 0; h <= HOURS_MAX; ++h) {
        s_hour_line[h] = hairline(s_grid, false, s_grid_w - HOUR_AXIS_W);
        s_hour_mark[h] = line(s_grid, Role::Small, Ink::Tertiary, HOUR_AXIS_W - space::s);
    }
    for (lv_obj_t *&head : s_day_head) {
        head = line(s_grid, Role::H3, Ink::Secondary, 100);
        lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
    }
    for (int i = 0; i < WEEK_EVENTS; ++i) {
        Block &block = s_block[i];
        block.root   = plain(s_grid);
        lv_obj_set_style_bg_opa(block.root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(block.root, BLOCK_RADIUS, 0);
        lv_obj_set_style_border_side(block.root, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(block.root, 3, 0);
        lv_obj_set_style_pad_left(block.root, BLOCK_PAD, 0);
        lv_obj_set_style_pad_top(block.root, BLOCK_PAD / 2, 0);
        block.time  = line(block.root, Role::Small, Ink::Primary, 60);
        block.title = text(block.root, Role::Small, "");
        lv_label_set_long_mode(block.title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(block.title, 0, fonts::small()->line_height);
        block.place = line(block.root, Role::Small, Ink::Secondary, 60);
        lv_obj_set_clickable(block.root, true);
        lv_obj_add_event_cb(block.root, on_event_tap, LV_EVENT_CLICKED, &s_week_event[i]);
        popover_source(block.root);
        lv_obj_set_hidden(block.root, true);
    }
    s_now = plain(s_grid);
    lv_obj_set_height(s_now, NOW_LINE_H);
    lv_obj_set_style_bg_opa(s_now, LV_OPA_COVER, 0);
    lv_obj_set_hidden(s_week, true);
}

// --- the page ------------------------------------------------------------------

void show_calendar()
{
    // Rebuilt every thirty seconds and on every fetch, but only while it shows;
    // coming to show draws it anew.
    if (s_card == nullptr || detail::s_page != detail::CALENDAR_PAGE) {
        return;
    }
    static KIT_IN_PSRAM ical::Event ahead[EVENTS_AHEAD];
    const int          count = ical::upcoming(ahead, EVENTS_AHEAD);
    const auto         now   = static_cast<std::int64_t>(detail::wall_now());
    const ical::Event *next  = count > 0 ? &ahead[0] : nullptr;

    // Asked for by main well before it starts.
    const bool want = next != nullptr && next->start > now && next->start - now < kJourneyAhead;
    pick_journey(want, now);
    show_next(next, now);
    const int also = show_foot(ahead, count, now);
    if (s_detailed) {
        show_week(now);
    } else {
        show_list(ahead, count, also, now);
    }
}

void toggle_clicked(lv_event_t *)
{
    s_detailed   = !s_detailed;
    s_week_shift = 0;
    lv_obj_set_hidden(s_overview, s_detailed);
    lv_obj_set_hidden(s_week, !s_detailed);
    show_calendar();
}
}  // namespace

std::uint32_t event_ink(std::uint8_t feed)
{
    switch (feed) {
        case FEED_LECTURES: return INK_LECTURES;
        case FEED_PRACTICALS: return INK_PRACTICALS;
        case FEED_EXAMS: return INK_EXAMS;
        case ical::kWorkFeed: return INK_WORK;
        default: return INK_OTHER;
    }
}

void event_clock(std::int64_t at, char *out, std::size_t size)
{
    const std::tm when = local(at);
    std::snprintf(out, size, "%02d:%02d", when.tm_hour, when.tm_min);
}

// What kind of event, and when: how long until it, or until it ends, on its
// day, and which day before that.
void event_kind(const ical::Event &event, std::int64_t now, char *out, std::size_t size)
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

void event_course(const ical::Event &event, char *out, std::size_t size)
{
    char session[ical::kSummaryMax];
    split_title(event.summary, out, size, session, sizeof(session));
}

void event_place(const ical::Event &event, char *out, std::size_t size) { short_place(place_of(event), out, size); }

void build_calendar(lv_obj_t *page)
{
    lv_style_init(&s_pressed);
    lv_style_set_bg_opa(&s_pressed, LV_OPA_COVER);
    restyle_pressed();

    s_overview = flow(page, LV_FLEX_FLOW_ROW, space::l);
    build_card(s_overview);
    build_coming_up(s_overview);
    build_week(page);
    on_restyle(s_overview, [](void *) {
        restyle_pressed();
        show_calendar();  // the week's blocks are mixed with the tiles' colour
    }, nullptr);

    lv_timer_create([](lv_timer_t *) { show_calendar(); }, REFRESH_MS, nullptr);
    subscribe(Topic::Calendar, detail::kNoView, show_calendar);
    subscribe(Topic::Page, detail::kNoView, show_calendar);
}

}  // namespace ui::kitfront
