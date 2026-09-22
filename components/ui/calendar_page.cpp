#include "calendar_page.h"

#include "fonts/units_font.h"
#include "ical.h"
#include "theme.h"
#include "travel.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace ui {
namespace {

// What fits under the hero card. Nobody reads past the end of the day on a wall.
constexpr int ROWS = 4;
constexpr int TRIPS = 2;

constexpr std::int32_t LEGEND_H = 34;

constexpr std::int32_t ROW_H   = 46;
constexpr std::int32_t TIME_W  = 84;
constexpr std::int32_t PLACE_W = 150;
constexpr std::int32_t GAP     = 10;
constexpr std::int32_t MARK_W  = 4;
constexpr std::int32_t HERO_H  = 336;

// Fixed rather than drawn from the theme: which feed an event came from is not
// a matter of taste, and the accent colour moves.
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

lv_obj_t *s_hero_when  = nullptr;
lv_obj_t *s_hero_title = nullptr;
lv_obj_t *s_hero_where = nullptr;
lv_obj_t *s_hero_in    = nullptr;
struct LegView {
    lv_obj_t *pill  = nullptr;
    lv_obj_t *mode  = nullptr;
    lv_obj_t *text  = nullptr;
};

struct TripView {
    lv_obj_t *root   = nullptr;
    lv_obj_t *leave  = nullptr;
    lv_obj_t *arrive = nullptr;
    LegView   legs[travel::kLegsMax];
};

TripView  s_trip[TRIPS];
lv_obj_t *s_travel_none = nullptr;
Row       s_row[ROWS];

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

Row make_row(lv_obj_t *parent, std::int32_t width)
{
    Row row;
    row.root = lv_obj_create(parent);
    lv_obj_set_size(row.root, width, ROW_H);
    theme::style_panel(row.root, theme::panel, 10);
    lv_obj_set_style_pad_hor(row.root, GAP, 0);
    lv_obj_set_style_border_side(row.root, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(row.root, MARK_W, 0);
    lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_hidden(row.root, true);
    quiet(row.root);

    row.time = theme::make_label(row.root, "", theme::text, fonts::size_20());
    lv_obj_set_width(row.time, TIME_W);

    row.title = theme::make_label(row.root, "", theme::text, fonts::size_20());
    lv_obj_set_flex_grow(row.title, 1);
    lv_obj_set_height(row.title, fonts::size_20()->line_height);
    lv_label_set_long_mode(row.title, LV_LABEL_LONG_MODE_DOTS);

    row.place = theme::make_label(row.root, "", theme::secondary, fonts::size_20());
    lv_obj_set_width(row.place, PLACE_W);
    lv_obj_set_style_text_align(row.place, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(row.place, LV_LABEL_LONG_MODE_DOTS);
    return row;
}

void clock_of(std::int64_t at, char *out, std::size_t size)
{
    std::tm     when{};
    const auto  stamp = static_cast<std::time_t>(at);
    localtime_r(&stamp, &when);
    std::snprintf(out, size, "%02d:%02d", when.tm_hour, when.tm_min);
}

// How to get there, under the event itself: two ways, each with when to go, when
// you are there, and where every leg puts you down.
void show_travel(std::int64_t start, std::int64_t now)
{
    if (s_trip[0].root == nullptr) {
        return;
    }
    static travel::Option options[travel::kOptionsMax];
    const int found = start == 0 ? 0 : travel::options(options, travel::kOptionsMax);

    int shown = 0;
    for (int i = 0; i < found && shown < TRIPS; ++i) {
        const travel::Option &option = options[i];
        if (option.leave < now) {
            continue;
        }
        TripView &trip = s_trip[shown];

        char leave[16];
        char arrive[16];
        clock_of(option.leave, leave, sizeof(leave));
        clock_of(option.arrive, arrive, sizeof(arrive));

        char text[48];
        const int minutes = static_cast<int>((option.leave - now) / 60);
        if (minutes <= 0) {
            std::snprintf(text, sizeof(text), "LEAVE %s  now", leave);
        } else if (minutes < 60) {
            std::snprintf(text, sizeof(text), "LEAVE %s  in %d min", leave, minutes);
        } else {
            std::snprintf(text, sizeof(text), "LEAVE %s  in %dh%02dm", leave, minutes / 60,
                          minutes % 60);
        }
        theme::set_text(trip.leave, text);

        std::snprintf(text, sizeof(text), "arrive %s", arrive);
        theme::set_text(trip.arrive, text);

        for (int l = 0; l < travel::kLegsMax; ++l) {
            const bool real = l < option.leg_count;
            lv_obj_set_hidden(trip.legs[l].pill, !real);
            lv_obj_set_hidden(trip.legs[l].text, !real);
            if (!real) {
                continue;
            }
            const travel::Leg &leg = option.legs[l];
            const bool train = std::strcmp(leg.mode, "train") == 0;

            theme::set_text(trip.legs[l].mode, train ? "TRAIN" : "BUS");

            char from[16];
            char to[16];
            clock_of(leg.depart, from, sizeof(from));
            clock_of(leg.arrive, to, sizeof(to));
            std::snprintf(text, sizeof(text), "%s", leg.line);
            char line[128];
            std::snprintf(line, sizeof(line), "%-4s %s %s  to  %s %s", leg.line, leg.from, from,
                          leg.to, to);
            theme::set_text(trip.legs[l].text, line);
        }

        lv_obj_set_hidden(trip.root, false);
        ++shown;
    }

    for (int i = shown; i < TRIPS; ++i) {
        lv_obj_set_hidden(s_trip[i].root, true);
    }
    theme::set_text(s_travel_none, shown > 0 ? "" : "No way there yet");
}

}  // namespace

void show_calendar()
{
    if (s_hero_title == nullptr) {
        return;
    }

    static ical::Event ahead[ROWS + 1];
    const int          count = ical::upcoming(ahead, ROWS + 1);
    const auto         now   = static_cast<std::int64_t>(std::time(nullptr));

    // Wide enough to cover the night before: the train is worth knowing then
    // even though the bus is not, and the service answers with the train alone.
    constexpr std::int64_t ASK_WITHIN = 18 * 3600;
    travel::want(count > 0 && ahead[0].start - now < ASK_WITHIN ? ahead[0].start : 0);
    show_travel(count > 0 ? ahead[0].start : 0, now);

    if (count == 0) {
        theme::set_text(s_hero_in, "");
        theme::set_text(s_hero_title, "Nothing scheduled");
        theme::set_text(s_hero_when, "");
        theme::set_text(s_hero_where, "");
    } else {
        const ical::Event &first   = ahead[0];
        const int          minutes = static_cast<int>((first.start - now) / 60);

        char text[64];
        if (minutes <= 0) {
            std::snprintf(text, sizeof(text), "NOW");
        } else if (minutes < 60) {
            std::snprintf(text, sizeof(text), "IN %d MIN", minutes);
        } else {
            std::snprintf(text, sizeof(text), "IN %dH %02dM", minutes / 60, minutes % 60);
        }
        theme::set_text(s_hero_in, text);
        theme::set_text(s_hero_title, first.summary);

        std::tm    from{};
        std::tm    to{};
        const auto a = static_cast<std::time_t>(first.start);
        const auto b = static_cast<std::time_t>(first.end);
        localtime_r(&a, &from);
        localtime_r(&b, &to);
        std::snprintf(text, sizeof(text), "%02d:%02d - %02d:%02d   %s", from.tm_hour, from.tm_min,
                      to.tm_hour, to.tm_min, ical::feed_name(first.feed));
        theme::set_text(s_hero_when, text);
        theme::set_text(s_hero_where, first.location);
    }

    for (int i = 0; i < ROWS; ++i) {
        const int at = i + 1;
        if (at >= count) {
            lv_obj_set_hidden(s_row[i].root, true);
            continue;
        }
        const auto when = static_cast<std::time_t>(ahead[at].start);
        std::tm    local{};
        localtime_r(&when, &local);

        char clock[16];
        std::snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        theme::set_text(s_row[i].time, clock);
        theme::set_text(s_row[i].title, ahead[at].summary);
        theme::set_text(s_row[i].place, ahead[at].location);
        lv_obj_set_style_border_color(s_row[i].root, lv_color_hex(feed_ink(ahead[at].feed)), 0);
        lv_obj_set_hidden(s_row[i].root, false);
    }
}

void build_calendar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    lv_obj_t *title = theme::make_page_title(page, "Calendar");
    lv_obj_set_pos(title, 0, 0);

    constexpr std::int32_t TOP = 58;

    lv_obj_t *hero = lv_obj_create(page);
    lv_obj_set_pos(hero, 0, TOP);
    lv_obj_set_size(hero, width, HERO_H);
    theme::style_panel(hero, theme::panel, 16);
    lv_obj_set_style_pad_all(hero, 20, 0);
    quiet(hero);

    s_hero_in = theme::make_accent_label(hero, "", fonts::size_20());
    lv_obj_set_pos(s_hero_in, 0, 0);

    s_hero_title = theme::make_label(hero, "", theme::text, fonts::size_28());
    lv_obj_set_pos(s_hero_title, 0, 34);
    lv_obj_set_width(s_hero_title, width - 40);
    lv_obj_set_height(s_hero_title, fonts::size_28()->line_height);
    lv_label_set_long_mode(s_hero_title, LV_LABEL_LONG_MODE_DOTS);

    s_hero_when = theme::make_label(hero, "", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_hero_when, 0, 86);

    s_hero_where = theme::make_label(hero, "", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_hero_where, 0, 116);

    const std::int32_t inner = width - 40;

    lv_obj_t *rule = lv_obj_create(hero);
    lv_obj_set_pos(rule, 0, 152);
    lv_obj_set_size(rule, inner, 1);
    theme::style_panel(rule, theme::panel_light, 0);
    quiet(rule);

    s_travel_none = theme::make_label(hero, "", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_travel_none, 0, 168);
    quiet(s_travel_none);

    for (int i = 0; i < TRIPS; ++i) {
        TripView &trip = s_trip[i];
        trip.root = lv_obj_create(hero);
        lv_obj_set_pos(trip.root, 0, 166 + i * 84);
        lv_obj_set_size(trip.root, inner, 78);
        lv_obj_set_style_bg_opa(trip.root, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(trip.root, 0, 0);
        lv_obj_set_style_pad_all(trip.root, 0, 0);
        lv_obj_set_hidden(trip.root, true);
        quiet(trip.root);

        trip.leave = theme::make_accent_label(trip.root, "", fonts::size_20());
        lv_obj_set_pos(trip.leave, 0, 0);

        trip.arrive = theme::make_label(trip.root, "", theme::secondary, fonts::size_20());
        lv_obj_align(trip.arrive, LV_ALIGN_TOP_RIGHT, 0, 0);

        for (int l = 0; l < travel::kLegsMax; ++l) {
            LegView &leg = trip.legs[l];
            leg.pill = lv_obj_create(trip.root);
            lv_obj_set_size(leg.pill, 62, 22);
            lv_obj_set_pos(leg.pill, 0, 30 + l * 24);
            theme::style_panel(leg.pill, theme::panel_light, 11);
            lv_obj_set_hidden(leg.pill, true);
            quiet(leg.pill);

            leg.mode = theme::make_label(leg.pill, "", theme::secondary, fonts::size_16());
            lv_obj_center(leg.mode);

            leg.text = theme::make_label(trip.root, "", theme::text, fonts::size_16());
            lv_obj_set_pos(leg.text, 72, 32 + l * 24);
            lv_obj_set_width(leg.text, inner - 72);
            lv_label_set_long_mode(leg.text, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_hidden(leg.text, true);
        }
    }

    lv_obj_t *rest = lv_obj_create(page);
    lv_obj_set_pos(rest, 0, TOP + HERO_H + 16);
    lv_obj_set_size(rest, width, height - TOP - HERO_H - 16 - LEGEND_H);
    lv_obj_set_style_bg_opa(rest, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rest, 0, 0);
    lv_obj_set_style_pad_all(rest, 0, 0);
    lv_obj_set_style_pad_row(rest, 6, 0);
    lv_obj_set_flex_flow(rest, LV_FLEX_FLOW_COLUMN);
    quiet(rest);

    for (int i = 0; i < ROWS; ++i) {
        s_row[i] = make_row(rest, width);
    }

    lv_obj_t *legend = lv_obj_create(page);
    lv_obj_set_pos(legend, 0, height - LEGEND_H);
    lv_obj_set_size(legend, width, LEGEND_H);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(legend, 0, 0);
    lv_obj_set_style_pad_all(legend, 0, 0);
    lv_obj_set_style_pad_column(legend, 8, 0);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(legend, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    quiet(legend);

    for (int i = 0; i < ical::kFeedCount; ++i) {
        lv_obj_t *chip = lv_obj_create(legend);
        lv_obj_set_size(chip, 14, 14);
        theme::style_panel(chip, feed_ink(static_cast<std::uint8_t>(i)), 7);
        quiet(chip);

        lv_obj_t *name = theme::make_label(legend, ical::feed_name(static_cast<std::uint8_t>(i)),
                                           theme::secondary, fonts::size_16());
        lv_obj_set_style_margin_right(name, 18, 0);
        quiet(name);
    }

    // An event that has finished should leave the page, and the countdown has to
    // tick down; neither arrives with a fetch.
    lv_timer_create([](lv_timer_t *) { show_calendar(); }, 30000, nullptr);

    show_calendar();
}

}  // namespace ui
