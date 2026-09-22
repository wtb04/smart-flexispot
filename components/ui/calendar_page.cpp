#include "calendar_page.h"

#include "fonts/units_font.h"
#include "ical.h"
#include "theme.h"
#include "travel.h"

#include <cstdio>
#include <ctime>

namespace ui {
namespace {

// What fits under the hero card. Nobody reads past the end of the day on a wall.
constexpr int ROWS = 8;

constexpr std::int32_t ROW_H   = 46;
constexpr std::int32_t TIME_W  = 84;
constexpr std::int32_t PLACE_W = 150;
constexpr std::int32_t GAP     = 10;
constexpr std::int32_t MARK_W  = 4;
constexpr std::int32_t HERO_H  = 168;

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
lv_obj_t *s_hero_travel = nullptr;
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

// One line under the hero: when to leave, and what to take.
void show_travel(std::int64_t start, std::int64_t now)
{
    if (s_hero_travel == nullptr) {
        return;
    }
    static travel::Option options[travel::kOptionsMax];
    const int found = start == 0 ? 0 : travel::options(options, travel::kOptionsMax);

    const travel::Option *best = nullptr;
    for (int i = 0; i < found; ++i) {
        if (options[i].leave >= now) {
            best = &options[i];
            break;
        }
    }
    if (best == nullptr) {
        theme::set_text(s_hero_travel, "");
        return;
    }

    std::tm when{};
    const auto at = static_cast<std::time_t>(best->leave);
    localtime_r(&at, &when);

    char text[96];
    int  used = std::snprintf(text, sizeof(text), "Leave %02d:%02d", when.tm_hour, when.tm_min);
    for (int i = 0; i < best->leg_count && used < static_cast<int>(sizeof(text)) - 1; ++i) {
        used += std::snprintf(text + used, sizeof(text) - static_cast<std::size_t>(used),
                              "  %s %s", best->legs[i].mode, best->legs[i].line);
    }
    theme::set_text(s_hero_travel, text);
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

    constexpr std::int64_t ASK_WITHIN = 4 * 3600;
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

    s_hero_travel = theme::make_accent_label(hero, "", fonts::size_20());
    lv_obj_align(s_hero_travel, LV_ALIGN_TOP_RIGHT, 0, 116);

    lv_obj_t *rest = lv_obj_create(page);
    lv_obj_set_pos(rest, 0, TOP + HERO_H + 16);
    lv_obj_set_size(rest, width, height - TOP - HERO_H - 16);
    lv_obj_set_style_bg_opa(rest, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rest, 0, 0);
    lv_obj_set_style_pad_all(rest, 0, 0);
    lv_obj_set_style_pad_row(rest, 6, 0);
    lv_obj_set_flex_flow(rest, LV_FLEX_FLOW_COLUMN);
    quiet(rest);

    for (int i = 0; i < ROWS; ++i) {
        s_row[i] = make_row(rest, width);
    }

    // An event that has finished should leave the page, and the countdown has to
    // tick down; neither arrives with a fetch.
    lv_timer_create([](lv_timer_t *) { show_calendar(); }, 30000, nullptr);

    show_calendar();
}

}  // namespace ui
