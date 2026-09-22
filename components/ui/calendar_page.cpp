#include "calendar_page.h"

#include "fonts/units_font.h"
#include "ical.h"
#include "theme.h"
#include "travel_icons.h"
#include "travel.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace ui {
namespace {

constexpr int ROWS  = 4;

// One scale, and the layout is flex all the way down, so spacing comes from
// these four numbers rather than from offsets typed into each object.
constexpr std::int32_t PAD   = 20;  // inside a card
constexpr std::int32_t GAP   = 16;  // between cards, and between blocks
constexpr std::int32_t STEP  = 8;   // between lines of a block
constexpr std::int32_t MARK  = 4;   // the colour down a row's edge

constexpr std::int32_t ROW_H  = 48;
constexpr std::int32_t HERO_H = 250;
constexpr std::int32_t KEY_H  = 22;
constexpr std::int32_t ICON   = 22;

constexpr std::int32_t TIME_W  = 78;
constexpr std::int32_t STOP_W  = 112;
constexpr std::int32_t PLACE_W = 150;

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

constexpr int POINTS = travel::kLegsMax + 1;

struct PointView {
    lv_obj_t *root = nullptr;
    lv_obj_t *when = nullptr;
    lv_obj_t *name = nullptr;
};

struct HopView {
    lv_obj_t *root = nullptr;
    lv_obj_t *icon = nullptr;
};

lv_obj_t *s_when   = nullptr;
lv_obj_t *s_title  = nullptr;
lv_obj_t *s_span   = nullptr;
lv_obj_t *s_where  = nullptr;
lv_obj_t *s_trip   = nullptr;   // the whole getting-there band
PointView s_point[POINTS];
HopView   s_hop[travel::kLegsMax];
lv_obj_t *s_lead  = nullptr;
lv_obj_t *s_after = nullptr;
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

lv_obj_t *bare(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    quiet(box);
    return box;
}

lv_obj_t *heading(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = theme::make_label(parent, text, theme::secondary, fonts::size_16());
    lv_obj_set_style_text_letter_space(label, 2, 0);
    lv_obj_set_style_text_opa(label, LV_OPA_70, 0);
    quiet(label);
    return label;
}

void clock_of(std::int64_t at, char *out, std::size_t size)
{
    std::tm    when{};
    const auto stamp = static_cast<std::time_t>(at);
    localtime_r(&stamp, &when);
    std::snprintf(out, size, "%02d:%02d", when.tm_hour, when.tm_min);
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
        theme::set_text(s_when, "");
        theme::set_text(s_title, "Nothing scheduled");
        theme::set_text(s_span, "");
        theme::set_text(s_where, "");
    } else {
        const ical::Event &first   = ahead[0];
        const int          minutes = static_cast<int>((first.start - now) / 60);
        if (minutes <= 0) {
            std::snprintf(text, sizeof(text), "NOW");
        } else if (minutes < 60) {
            std::snprintf(text, sizeof(text), "IN %d MIN", minutes);
        } else {
            std::snprintf(text, sizeof(text), "IN %dH %02dM", minutes / 60, minutes % 60);
        }
        theme::set_text(s_when, text);
        theme::set_text(s_title, first.summary);

        char from[16];
        char to[16];
        clock_of(first.start, from, sizeof(from));
        clock_of(first.end, to, sizeof(to));
        std::snprintf(text, sizeof(text), "%s - %s", from, to);
        theme::set_text(s_span, text);
        theme::set_text(s_where, first.location);
    }

    // --- how to get there
    static travel::Option options[travel::kOptionsMax];
    const int found = count == 0 ? 0 : travel::options(options, travel::kOptionsMax);

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

    for (int i = 0; i < POINTS; ++i) {
        lv_obj_set_hidden(s_point[i].root, true);
    }
    for (int i = 0; i < travel::kLegsMax; ++i) {
        lv_obj_set_hidden(s_hop[i].root, true);
    }

    const bool travelling = going != nullptr;
    lv_obj_set_hidden(s_trip, !travelling);

    if (travelling) {
        const int minutes = static_cast<int>((going->leave - now) / 60);
        if (minutes <= 0) {
            std::snprintf(text, sizeof(text), "leave now");
        } else if (minutes < 60) {
            std::snprintf(text, sizeof(text), "leave in %d min", minutes);
        } else {
            std::snprintf(text, sizeof(text), "leave in %dh %02dm", minutes / 60, minutes % 60);
        }
        theme::set_text(s_lead, text);
        theme::set_text_color(s_lead, going->cancelled ? theme::red : theme::primary);

        // A place the journey passes through is one place, whatever it is the end
        // of and the start of. Where it is both, the two times are told apart.
        const int legs = going->leg_count;
        for (int i = 0; i <= legs && i < POINTS; ++i) {
            const char *name = i < legs ? going->legs[i].from : going->legs[legs - 1].to;
            char        into[16];
            char        away[16];
            into[0] = '\0';
            away[0] = '\0';
            if (i > 0) {
                clock_of(going->legs[i - 1].arrive, into, sizeof(into));
            }
            if (i < legs) {
                clock_of(going->legs[i].depart, away, sizeof(away));
            }

            if (into[0] != '\0' && away[0] != '\0') {
                std::snprintf(text, sizeof(text), "%s dep %s", into, away);
            } else if (away[0] != '\0') {
                std::snprintf(text, sizeof(text), "dep %s", away);
            } else {
                std::snprintf(text, sizeof(text), "arr %s", into);
            }
            theme::set_text(s_point[i].when, text);
            theme::set_text(s_point[i].name, name);
            lv_obj_set_hidden(s_point[i].root, false);

            if (i < legs) {
                const travel::Leg &leg = going->legs[i];
                lv_image_set_src(s_hop[i].icon, std::strcmp(leg.mode, "train") == 0
                                                    ? &icons::train_icon
                                                    : &icons::bus_icon);
                lv_obj_set_style_image_recolor(
                    s_hop[i].icon, lv_color_hex(leg.cancelled ? theme::red : theme::secondary), 0);
                lv_obj_set_hidden(s_hop[i].root, false);
            }
        }

        if (after != nullptr) {
            char next[16];
            clock_of(after->leave, next, sizeof(next));
            std::snprintf(text, sizeof(text), "or the one after at %s", next);
            theme::set_text(s_after, text);
        } else {
            theme::set_text(s_after, "");
        }
    }

    // --- what follows
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
}

void build_calendar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    (void)width;
    (void)height;

    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, STEP, 0);

    // --- what is next, and under it how to get there
    lv_obj_t *hero = lv_obj_create(page);
    lv_obj_set_width(hero, LV_PCT(100));
    lv_obj_set_height(hero, LV_SIZE_CONTENT);
    theme::style_panel(hero, theme::panel_light, 16);
    lv_obj_set_style_pad_all(hero, PAD, 0);
    lv_obj_set_style_pad_row(hero, STEP, 0);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    quiet(hero);

    heading(hero, "NEXT");

    s_title = theme::make_label(hero, "", theme::text, fonts::size_28());
    lv_obj_set_width(s_title, LV_PCT(100));
    lv_obj_set_height(s_title, fonts::size_28()->line_height);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *meta = bare(hero, 0, 0, 0, 0);
    lv_obj_set_width(meta, LV_PCT(100));
    lv_obj_set_height(meta, fonts::size_20()->line_height);
    lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(meta, GAP, 0);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_span  = theme::make_label(meta, "", theme::secondary, fonts::size_20());
    s_where = theme::make_label(meta, "", theme::secondary, fonts::size_20());
    lv_obj_set_flex_grow(s_where, 1);
    lv_obj_set_height(s_where, fonts::size_20()->line_height);
    lv_label_set_long_mode(s_where, LV_LABEL_LONG_MODE_DOTS);

    s_when = theme::make_accent_label(meta, "", fonts::size_20());

    // The journey only takes room when there is one. A half-empty card with a
    // rule down the middle said nothing most of the day.
    s_trip = bare(hero, 0, 0, 0, 0);
    lv_obj_set_width(s_trip, LV_PCT(100));
    lv_obj_set_height(s_trip, LV_SIZE_CONTENT);
    lv_obj_set_style_margin_top(s_trip, STEP, 0);
    lv_obj_set_flex_flow(s_trip, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_trip, 2, 0);

    lv_obj_t *band = bare(s_trip, 0, 0, 0, 0);
    lv_obj_set_width(band, LV_PCT(100));
    lv_obj_set_height(band, fonts::size_16()->line_height);
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    heading(band, "GETTING THERE");
    s_lead = theme::make_accent_label(band, "", fonts::size_16());

    for (int i = 0; i < POINTS; ++i) {
        PointView &point = s_point[i];
        point.root       = bare(s_trip, 0, 0, 0, 0);
        lv_obj_set_width(point.root, LV_PCT(100));
        lv_obj_set_height(point.root, fonts::size_20()->line_height);
        lv_obj_set_flex_flow(point.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(point.root, STEP + 2, 0);
        lv_obj_set_flex_align(point.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_hidden(point.root, true);

        point.when = theme::make_label(point.root, "", theme::text, fonts::size_20());
        lv_obj_set_width(point.when, STOP_W);

        point.name = theme::make_label(point.root, "", theme::secondary, fonts::size_20());
        lv_obj_set_flex_grow(point.name, 1);
        lv_obj_set_height(point.name, fonts::size_20()->line_height);
        lv_label_set_long_mode(point.name, LV_LABEL_LONG_MODE_DOTS);

        if (i >= travel::kLegsMax) {
            continue;
        }
        HopView &hop = s_hop[i];
        hop.root     = bare(s_trip, 0, 0, 0, 0);
        lv_obj_set_width(hop.root, LV_PCT(100));
        lv_obj_set_height(hop.root, ICON - 6);
        lv_obj_set_style_pad_left(hop.root, STOP_W - ICON / 2, 0);
        lv_obj_set_flex_flow(hop.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hop.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_hidden(hop.root, true);

        hop.icon = lv_image_create(hop.root);
        lv_image_set_src(hop.icon, &icons::bus_icon);
        lv_obj_set_style_image_recolor(hop.icon, lv_color_hex(theme::secondary), 0);
        lv_obj_set_style_image_recolor_opa(hop.icon, LV_OPA_COVER, 0);
        quiet(hop.icon);
    }

    s_after = theme::make_label(s_trip, "", theme::secondary, fonts::size_16());
    quiet(s_after);

    // --- what follows it
    heading(page, "LATER");

    lv_obj_t *rest = bare(page, 0, 0, 0, 0);
    lv_obj_set_width(rest, LV_PCT(100));
    lv_obj_set_flex_grow(rest, 1);
    lv_obj_set_flex_flow(rest, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rest, STEP, 0);
    lv_obj_set_scrollable(rest, true);
    lv_obj_set_scroll_dir(rest, LV_DIR_VER);

    for (int i = 0; i < ROWS; ++i) {
        Row &row = s_row[i];
        row.root = lv_obj_create(rest);
        lv_obj_set_width(row.root, LV_PCT(100));
        lv_obj_set_height(row.root, ROW_H);
        theme::style_panel(row.root, theme::panel, 10);
        lv_obj_set_style_pad_hor(row.root, PAD, 0);
        lv_obj_set_style_pad_column(row.root, GAP, 0);
        lv_obj_set_style_border_side(row.root, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(row.root, MARK, 0);
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
    }

    // --- what the colours mean
    lv_obj_t *key = bare(page, 0, 0, 0, 0);
    lv_obj_set_width(key, LV_PCT(100));
    lv_obj_set_height(key, KEY_H);
    lv_obj_set_style_pad_column(key, STEP, 0);
    lv_obj_set_flex_flow(key, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(key, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < ical::kFeedCount; ++i) {
        lv_obj_t *chip = lv_obj_create(key);
        lv_obj_set_size(chip, 12, 12);
        theme::style_panel(chip, feed_ink(static_cast<std::uint8_t>(i)), 6);
        quiet(chip);

        lv_obj_t *name = theme::make_label(key, ical::feed_name(static_cast<std::uint8_t>(i)),
                                           theme::secondary, fonts::size_16());
        lv_obj_set_style_margin_right(name, GAP, 0);
        quiet(name);
    }

    lv_timer_create([](lv_timer_t *) { show_calendar(); }, 30000, nullptr);
    show_calendar();
}

}  // namespace ui
