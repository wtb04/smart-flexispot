#include "calendar_page.h"

#include "fonts/units_font.h"
#include "ical.h"
#include "theme.h"
#include "travel.h"
#include "travel_icons.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace ui {
namespace {

namespace space  = theme::space;
namespace radius = theme::radius;

constexpr int ROWS   = 4;
constexpr int POINTS = travel::kLegsMax + 1;

constexpr std::int32_t ROW_H   = 48;
constexpr std::int32_t ICON    = 22;
constexpr std::int32_t TIME_W  = 78;
constexpr std::int32_t PLACE_W = 150;
constexpr std::int32_t STOP_W  = 132;
constexpr std::int32_t MARK    = 4;
constexpr std::int32_t KEY_H   = 22;

constexpr std::uint32_t INK_LECTURES   = 0x74c97a;
constexpr std::uint32_t INK_PRACTICALS = 0x63a9e8;
constexpr std::uint32_t INK_EXAMS      = 0xb07ce0;
constexpr std::uint32_t INK_OTHER      = 0xb0805a;

struct Row {
    lv_obj_t *root  = nullptr;
    lv_obj_t *time  = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *place = nullptr;
};

struct Stop {
    lv_obj_t *root = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *when = nullptr;
    lv_obj_t *name = nullptr;
};

lv_obj_t *s_countdown = nullptr;
lv_obj_t *s_start     = nullptr;
lv_obj_t *s_title     = nullptr;
lv_obj_t *s_meta      = nullptr;
lv_obj_t *s_journey   = nullptr;
lv_obj_t *s_leave     = nullptr;
lv_obj_t *s_then      = nullptr;
Stop      s_stop[POINTS];
Row       s_row[ROWS];
lv_obj_t *s_rest = nullptr;

std::uint32_t feed_ink(std::uint8_t feed)
{
    switch (feed) {
        case 0: return INK_LECTURES;
        case 1: return INK_PRACTICALS;
        case 2: return INK_EXAMS;
        default: return INK_OTHER;
    }
}

void quiet(lv_obj_t *obj)
{
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
}

// A row of things laid out left to right, with nothing drawn behind it.
lv_obj_t *line_of(lv_obj_t *parent, std::int32_t height, lv_flex_align_t main)
{
    lv_obj_t *line = lv_obj_create(parent);
    theme::style_panel(line, theme::panel, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_width(line, LV_PCT(100));
    lv_obj_set_height(line, height);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, space::s + 2, 0);
    quiet(line);
    return line;
}

lv_obj_t *column_of(lv_obj_t *parent, std::int32_t gap)
{
    lv_obj_t *column = lv_obj_create(parent);
    theme::style_panel(column, theme::panel, 0);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_width(column, LV_PCT(100));
    lv_obj_set_height(column, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, gap, 0);
    quiet(column);
    return column;
}

void clock_of(std::int64_t at, char *out, std::size_t size)
{
    std::tm    when{};
    const auto stamp = static_cast<std::time_t>(at);
    localtime_r(&stamp, &when);
    std::snprintf(out, size, "%02d:%02d", when.tm_hour, when.tm_min);
}

void span_of(std::int64_t from_now, char *out, std::size_t size)
{
    const int minutes = static_cast<int>(from_now / 60);
    if (minutes <= 0) {
        std::snprintf(out, size, "now");
    } else if (minutes < 60) {
        std::snprintf(out, size, "in %d min", minutes);
    } else {
        std::snprintf(out, size, "in %dh %02dm", minutes / 60, minutes % 60);
    }
}

void show_journey(const ical::Event *next, std::int64_t now)
{
    static travel::Option options[travel::kOptionsMax];
    const int found = next == nullptr ? 0 : travel::options(options, travel::kOptionsMax);

    const travel::Option *going = nullptr;
    const travel::Option *after = nullptr;
    for (int i = 0; i < found; ++i) {
        if (options[i].leave < now) {
            continue;
        }
        if (going == nullptr) {
            going = &options[i];
        } else if (after == nullptr) {
            after = &options[i];
        }
    }

    lv_obj_set_hidden(s_journey, going == nullptr);
    if (going == nullptr) {
        return;
    }

    char text[96];
    char span[32];
    span_of(going->leave - now, span, sizeof(span));
    std::snprintf(text, sizeof(text), "leave %s", span);
    theme::set_text(s_leave, text);
    theme::set_text_color(s_leave, going->cancelled ? theme::red : theme::primary);

    // One row per place. The mode sits on the row it leaves from, and where a
    // journey changes, that place's arrival and departure share its row.
    const int legs = going->leg_count;
    for (int i = 0; i < POINTS; ++i) {
        const bool real = i <= legs;
        lv_obj_set_hidden(s_stop[i].root, !real);
        if (!real) {
            continue;
        }

        char into[16] = "";
        char away[16] = "";
        if (i > 0) {
            clock_of(going->legs[i - 1].arrive, into, sizeof(into));
        }
        if (i < legs) {
            clock_of(going->legs[i].depart, away, sizeof(away));
        }
        if (into[0] != '\0' && away[0] != '\0') {
            std::snprintf(text, sizeof(text), "%s  %s", into, away);
        } else {
            std::snprintf(text, sizeof(text), "%s", into[0] != '\0' ? into : away);
        }
        theme::set_text(s_stop[i].when, text);
        theme::set_text(s_stop[i].name, i < legs ? going->legs[i].from
                                                 : going->legs[legs - 1].to);

        const bool leaves = i < legs;
        lv_obj_set_style_image_opa(s_stop[i].icon, leaves ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (leaves) {
            const travel::Leg &leg = going->legs[i];
            lv_image_set_src(s_stop[i].icon, std::strcmp(leg.mode, "train") == 0
                                                 ? &icons::train_icon
                                                 : &icons::bus_icon);
            lv_obj_set_style_image_recolor(
                s_stop[i].icon, lv_color_hex(leg.cancelled ? theme::red : theme::secondary), 0);
        }
        theme::set_text_color(s_stop[i].when,
                              i < legs && going->legs[i].cancelled ? theme::red : theme::text);
    }

    if (after != nullptr) {
        char next[16];
        clock_of(after->leave, next, sizeof(next));
        std::snprintf(text, sizeof(text), "next one at %s", next);
        theme::set_text(s_then, text);
    } else {
        theme::set_text(s_then, "");
    }
}

}  // namespace

void show_calendar()
{
    if (s_title == nullptr) {
        return;
    }

    static ical::Event ahead[ROWS + 1];
    const int          count = ical::upcoming(ahead, ROWS + 1);
    const auto         now   = static_cast<std::int64_t>(std::time(nullptr));

    constexpr std::int64_t ASK_WITHIN = 18 * 3600;
    travel::want(count > 0 && ahead[0].start - now < ASK_WITHIN ? ahead[0].start : 0);

    char text[96];
    if (count == 0) {
        theme::set_text(s_countdown, "");
        theme::set_text(s_start, "");
        theme::set_text(s_title, "Nothing scheduled");
        theme::set_text(s_meta, "");
    } else {
        const ical::Event &first = ahead[0];
        char               span[32];
        span_of(first.start - now, span, sizeof(span));
        theme::set_text(s_countdown, span);
        theme::set_text(s_title, first.summary);

        char from[16];
        char to[16];
        clock_of(first.start, from, sizeof(from));
        clock_of(first.end, to, sizeof(to));
        theme::set_text(s_start, from);
        std::snprintf(text, sizeof(text), "%s  \xc2\xb7  until %s", first.location, to);
        theme::set_text(s_meta, text);
    }

    show_journey(count > 0 ? &ahead[0] : nullptr, now);

    for (int i = 0; i < ROWS; ++i) {
        const int at = i + 1;
        if (at >= count) {
            lv_obj_set_hidden(s_row[i].root, true);
            continue;
        }
        char clock[16];
        clock_of(ahead[at].start, clock, sizeof(clock));
        theme::set_text(s_row[i].time, clock);
        theme::set_text(s_row[i].title, ahead[at].summary);
        theme::set_text(s_row[i].place, ahead[at].location);
        lv_obj_set_style_border_color(s_row[i].root, lv_color_hex(feed_ink(ahead[at].feed)), 0);
        lv_obj_set_hidden(s_row[i].root, false);
    }

    // A row is shown whole or not at all. How many fit depends on whether the
    // journey card is there, and a sliver of one reads as something broken.
    lv_obj_update_layout(s_rest);
    const std::int32_t room = lv_obj_get_content_height(s_rest);
    for (int i = 0; i < ROWS; ++i) {
        if (!lv_obj_has_flag(s_row[i].root, LV_OBJ_FLAG_HIDDEN) &&
            lv_obj_get_y(s_row[i].root) + lv_obj_get_height(s_row[i].root) > room) {
            lv_obj_set_hidden(s_row[i].root, true);
        }
    }
}

void build_calendar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    (void)width;
    (void)height;

    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, space::m, 0);

    // --- the next thing: the one card on this page to read first
    // What it is on the left, when on the right -- the way every calendar's
    // "up next" is laid out, so it reads without being learned.
    lv_obj_t *next = theme::make_focus_card(page);
    lv_obj_set_width(next, LV_PCT(100));
    lv_obj_set_height(next, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(next, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(next, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(next, space::l, 0);
    quiet(next);

    lv_obj_t *what = column_of(next, space::xs);
    lv_obj_set_width(what, 0);
    lv_obj_set_flex_grow(what, 1);
    theme::make_eyebrow(what, "NEXT");

    s_title = theme::make_label(what, "", theme::text, theme::type_title());
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_obj_set_height(s_title, theme::type_title()->line_height);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    s_meta = theme::make_label(what, "", theme::secondary, theme::type_body());

    lv_obj_t *when = column_of(next, 0);
    lv_obj_set_width(when, LV_SIZE_CONTENT);
    lv_obj_set_style_flex_cross_place(when, LV_FLEX_ALIGN_END, 0);
    lv_obj_set_flex_align(when, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    s_start = theme::make_label(when, "", theme::text, theme::type_display());
    s_countdown = theme::make_accent_label(when, "", theme::type_body());

    // --- how to get there, only when there is a way
    s_journey = theme::make_card(page);
    lv_obj_set_width(s_journey, LV_PCT(100));
    lv_obj_set_height(s_journey, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_journey, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_journey, space::s, 0);
    lv_obj_set_hidden(s_journey, true);
    quiet(s_journey);

    lv_obj_t *band =
        line_of(s_journey, theme::type_label()->line_height, LV_FLEX_ALIGN_SPACE_BETWEEN);
    theme::make_eyebrow(band, "GETTING THERE");
    s_leave = theme::make_eyebrow(band, "");

    lv_obj_t *stops = column_of(s_journey, space::xs);
    for (int i = 0; i < POINTS; ++i) {
        Stop &stop = s_stop[i];
        stop.root  = line_of(stops, ICON + 4, LV_FLEX_ALIGN_START);
        lv_obj_set_hidden(stop.root, true);

        stop.icon = lv_image_create(stop.root);
        lv_image_set_src(stop.icon, &icons::bus_icon);
        lv_obj_set_style_image_recolor(stop.icon, lv_color_hex(theme::secondary), 0);
        lv_obj_set_style_image_recolor_opa(stop.icon, LV_OPA_COVER, 0);
        quiet(stop.icon);

        stop.when = theme::make_label(stop.root, "", theme::text, theme::type_value());
        lv_obj_set_width(stop.when, STOP_W);

        stop.name = theme::make_label(stop.root, "", theme::secondary, theme::type_body());
        lv_obj_set_flex_grow(stop.name, 1);
        lv_obj_set_height(stop.name, theme::type_body()->line_height);
        lv_label_set_long_mode(stop.name, LV_LABEL_LONG_MODE_DOTS);
    }

    s_then = theme::make_label(s_journey, "", theme::secondary, theme::type_label());
    quiet(s_then);

    // --- what follows
    theme::make_eyebrow(page, "LATER");

    lv_obj_t *rest = column_of(page, space::s);
    s_rest         = rest;
    lv_obj_set_height(rest, 0);
    lv_obj_set_flex_grow(rest, 1);


    for (int i = 0; i < ROWS; ++i) {
        Row &row = s_row[i];
        row.root = lv_obj_create(rest);
        theme::style_panel(row.root, theme::panel_light, radius::row);
        lv_obj_set_width(row.root, LV_PCT(100));
        lv_obj_set_height(row.root, ROW_H);
        lv_obj_set_style_pad_hor(row.root, space::l, 0);
        lv_obj_set_style_pad_column(row.root, space::m, 0);
        lv_obj_set_style_border_side(row.root, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(row.root, MARK, 0);
        lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_hidden(row.root, true);
        quiet(row.root);

        row.time = theme::make_label(row.root, "", theme::text, theme::type_body());
        lv_obj_set_width(row.time, TIME_W);

        row.title = theme::make_label(row.root, "", theme::text, theme::type_body());
        lv_obj_set_flex_grow(row.title, 1);
        lv_obj_set_height(row.title, theme::type_body()->line_height);
        lv_label_set_long_mode(row.title, LV_LABEL_LONG_MODE_DOTS);

        row.place = theme::make_label(row.root, "", theme::secondary, theme::type_body());
        lv_obj_set_width(row.place, PLACE_W);
        lv_obj_set_style_text_align(row.place, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(row.place, LV_LABEL_LONG_MODE_DOTS);
    }

    // --- what the colours mean
    lv_obj_t *key = line_of(page, KEY_H, LV_FLEX_ALIGN_START);
    for (int i = 0; i < ical::kFeedCount; ++i) {
        lv_obj_t *chip = lv_obj_create(key);
        lv_obj_set_size(chip, 12, 12);
        theme::style_panel(chip, feed_ink(static_cast<std::uint8_t>(i)), radius::pill);
        quiet(chip);

        lv_obj_t *name = theme::make_label(key, ical::feed_name(static_cast<std::uint8_t>(i)),
                                           theme::secondary, theme::type_label());
        lv_obj_set_style_margin_right(name, space::m, 0);
        quiet(name);
    }

    lv_timer_create([](lv_timer_t *) { show_calendar(); }, 30000, nullptr);
    show_calendar();
}

}  // namespace ui
