// Home: a fixed grid of widgets that only show, a tap on some opening their
// page. The air from Home Assistant, the next event, the aircraft overhead,
// the desk and the focus timer.

#include "kit_front.h"

#include "focus_model.h"
#include "home_model.h"
#include "icons.h"
#include "ical.h"
#include "kit_fonts.h"
#include "radar.h"
#include "radar_model.h"
#include "room_model.h"
#include "topics.h"
#include "units.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <ctime>
#include <utility>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;

constexpr std::uint32_t REFRESH_MS = 30 * units::kMsPerSecond;
constexpr int           AFTER_ROWS = 4;
constexpr float         KM_PER_NM  = 1.852f;
constexpr float         OVERHEAD_KM = 80.0f;  // as far as the radar's page looks at first
constexpr std::uint32_t LOOKUP_EVERY_MS = 30 * units::kMsPerSecond;
constexpr std::int32_t  DOT = 10;

void set_text_if(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
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

lv_obj_t *line(lv_obj_t *parent, Role role, Ink ink, std::int32_t width)
{
    lv_obj_t *l = text(parent, role, "", ink);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, width, font_of(l)->line_height);
    return l;
}

// Where a smaller line beside a big figure stands on the figure's baseline.
std::int32_t baseline_y(Role big, Role small)
{
    const lv_font_t *fb = big == Role::ReadXL ? fonts::read_xl() : big == Role::ReadM ? fonts::read_m() : fonts::read_l();
    const lv_font_t *fs = small == Role::Small ? fonts::small() : small == Role::H3 ? fonts::h3() : fonts::body();
    return (fb->line_height - fb->base_line) - (fs->line_height - fs->base_line);
}

bool on_home() { return detail::s_page == detail::HOME_PAGE; }

// Home Assistant's labels come in capitals: "HUMIDITY" is "Humidity", while
// "CO2" and "VOC" stay as they are.
void sentence_case(const char *from, char *out, std::size_t size)
{
    std::snprintf(out, size, "%s", from);
    for (char *word = out; *word != '\0';) {
        char *end   = word;
        bool  plain_word = true;
        while (*end != '\0' && *end != ' ') {
            plain_word = plain_word && std::isupper(static_cast<unsigned char>(*end)) != 0;
            ++end;
        }
        if (plain_word && end - word > 3) {
            for (char *c = word + (word == out ? 1 : 0); c < end; ++c) {
                *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
            }
        }
        word = *end == ' ' ? end + 1 : end;
    }
}

// --- air ---------------------------------------------------------------------

// Each reading on a small gauge, its arc in the ink of the level Home Assistant
// gives it. Laid out from the widget's size: two by two, each gauge with its
// name and word beside it; away, with the widget across the page, four in a
// row, each name over its gauge and the word in the gauge's open foot.
struct Air {
    lv_obj_t *cell = nullptr, *caption = nullptr, *word = nullptr;
    Dial     *gauge = nullptr;
    bool      row = false;  // laid out four in a row
};
Air       s_air[kPillCount];
lv_obj_t *s_air_none = nullptr;

// How far round a reading goes, in tenths, by its name or else its unit: only
// the gauge's ends; how good it is comes from Home Assistant.
struct AirScale {
    const char  *name, *unit;
    std::int32_t min, max;
};
constexpr AirScale AIR_SCALES[] = {
    {"CO2", "ppm", 4000, 20000},
    {"VOC", "ppb", 0, 15000},
    {"Humidity", "%", 0, 1000},
    {"PM2.5", "\xc2\xb5g/m\xc2\xb3", 0, 750},
};

const AirScale *scale_of(const char *caption, const char *unit)
{
    for (const AirScale &scale : AIR_SCALES) {
        if (strcasecmp(caption, scale.name) == 0) {
            return &scale;
        }
    }
    for (const AirScale &scale : AIR_SCALES) {
        if (unit[0] != '\0' && std::strcmp(unit, scale.unit) == 0) {
            return &scale;
        }
    }
    return nullptr;
}

void word_of(Level level, const char *&word, Ink &ink)
{
    switch (level) {
        case Level::Good: word = "Good", ink = Ink::Green; break;
        case Level::Warn: word = "Fair", ink = Ink::Amber; break;
        case Level::Bad: word = "Poor", ink = Ink::Red; break;
        default: word = "", ink = Ink::Secondary; break;
    }
}

// "640 ppm" as a figure and its unit; "--" while there is no reading; anything
// else, "unavailable", as words.
bool split_value(const char *value, char *figure, std::size_t figure_size, char *unit, std::size_t unit_size)
{
    while (*value == ' ') {
        ++value;
    }
    std::size_t n      = 0;
    bool        digits = false;
    while (value[n] != '\0' && (std::isdigit(static_cast<unsigned char>(value[n])) != 0 || std::strchr("-+.,", value[n]) != nullptr)) {
        digits = digits || std::isdigit(static_cast<unsigned char>(value[n])) != 0;
        ++n;
    }
    if (value[0] == '\0' || std::strcmp(value, "--") == 0) {
        std::snprintf(figure, figure_size, "--");
        unit[0] = '\0';
        return true;
    }
    if (!digits) {
        return false;
    }
    std::snprintf(figure, figure_size, "%.*s", static_cast<int>(n), value);
    // Home Assistant passes on what a sensor reports, 63.15 % among it: a
    // whole number from ten up, one decimal under it
    if (char *point = std::strpbrk(figure, ".,")) {
        *point            = '.';
        const float level = std::strtof(figure, nullptr);
        std::snprintf(figure, figure_size, "%.*f", std::fabs(level) >= 10.0f ? 0 : 1, static_cast<double>(level));
    }
    const char *rest = value + n;
    while (*rest == ' ') {
        ++rest;
    }
    std::snprintf(unit, unit_size, "%s", rest);
    return true;
}

void paint_air()
{
    const detail::HomeState &home  = detail::home_state();
    int                      shown = 0;
    for (int i = 0; i < kPillCount; ++i) {
        Air                     &air   = s_air[i];
        const detail::PillState &state = home.pills[i];
        const bool               here  = state.label[0] != '\0';
        if (lv_obj_is_hidden(air.cell) == here) {
            lv_obj_set_hidden(air.cell, !here);
        }
        if (!here) {
            continue;
        }
        ++shown;
        char caption[sizeof(state.label)];
        sentence_case(state.label, caption, sizeof(caption));
        set_text_if(air.caption, caption);
        char       figure[sizeof(state.value)];
        char       unit[sizeof(state.value)];
        const bool number = split_value(state.value, figure, sizeof(figure), unit, sizeof(unit));
        const bool none   = !number || std::strcmp(figure, "--") == 0;
        set_text_if(air.gauge->figure(), number ? figure : "--");
        ink_if(air.gauge->figure(), none ? Ink::Tertiary : Ink::Primary);
        // what is not a figure, "unavailable", in words under the dash
        set_text_if(air.gauge->caption(), number ? unit : state.value);

        const char *word = "";
        Ink         ink  = Ink::Secondary;
        word_of(state.level, word, ink);
        set_text_if(air.word, word);
        ink_if(air.word, ink);

        const AirScale *scale = scale_of(caption, unit);
        if (none || scale == nullptr) {
            air.gauge->set_value(INT32_MIN);
            continue;
        }
        char dotted[sizeof(figure)];
        std::snprintf(dotted, sizeof(dotted), "%s", figure);
        for (char *c = dotted; *c != '\0'; ++c) {
            *c = *c == ',' ? '.' : *c;
        }
        air.gauge->set_range(scale->min, scale->max);
        air.gauge->set_value(static_cast<std::int32_t>(std::lround(std::strtof(dotted, nullptr) * 10.0f)));
        air.gauge->set_ink(state.level == Level::Neutral ? Ink::Primary : ink);
        air.gauge->set_faded(state.level == Level::Neutral);
    }
    lv_obj_set_hidden(s_air_none, shown > 0);
}

lv_obj_t *s_air_body = nullptr;

void place_cell(Air &air, std::int32_t w, std::int32_t h, bool row)
{
    const std::int32_t cap_h = fonts::small()->line_height, word_h = fonts::h3()->line_height;
    if (row) {
        // the name over the gauge, the word in its open foot
        const std::int32_t d = std::min(w, h - cap_h - space::s);
        air.gauge->set_size(d, d / 11);
        lv_obj_set_pos(air.gauge->obj(), (w - d) / 2, cap_h + space::s);
        lv_obj_set_pos(air.caption, 0, 0);
        lv_obj_set_pos(air.word, 0, cap_h + space::s + d - word_h);
        for (lv_obj_t *label : {air.caption, air.word}) {
            lv_obj_set_width(label, w);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        }
    } else {
        // the name and the word beside the gauge, level with its middle
        const std::int32_t text_w = std::max(widest(Role::Small, {"Humidity"}), widest(Role::H3, {"Good", "Fair", "Poor"}));
        const std::int32_t d      = std::min(h, w - space::m - text_w);
        air.gauge->set_size(d, d / 11);
        lv_obj_set_pos(air.gauge->obj(), 0, (h - d) / 2);
        const std::int32_t x = d + space::m, top = (h - cap_h - word_h) / 2;
        lv_obj_set_pos(air.caption, x, top);
        lv_obj_set_pos(air.word, x, top + cap_h);
        for (lv_obj_t *label : {air.caption, air.word}) {
            lv_obj_set_width(label, w - x);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        }
    }
    if (air.row != row) {
        air.row = row;
        lv_obj_set_style_text_font(air.gauge->figure(), row ? fonts::read_l() : fonts::read_m(), 0);
        lv_obj_set_width(air.gauge->figure(), widest(row ? Role::ReadL : Role::ReadM, {"8888.8"}));
    }
}

void place_air()
{
    const std::int32_t w = lv_obj_get_content_width(s_air_body), h = lv_obj_get_content_height(s_air_body);
    const bool         row = w > h * 5 / 2;  // across the page, away
    const int          across = row ? kPillCount : 2;
    const std::int32_t gap_x = row ? space::xl : space::xl, gap_y = space::l;
    const std::int32_t cell_w = (w - (across - 1) * gap_x) / across;
    const std::int32_t cell_h = row ? h : (h - gap_y) / 2;
    for (int i = 0; i < kPillCount; ++i) {
        Air &air = s_air[i];
        lv_obj_set_size(air.cell, cell_w, cell_h);
        lv_obj_set_pos(air.cell, (i % across) * (cell_w + gap_x), (i / across) * (cell_h + gap_y));
        place_cell(air, cell_w, cell_h, row);
    }
    lv_obj_set_size(s_air_none, w, h);
}

void build_air(lv_obj_t *grid)
{
    s_air_body = widget(grid, Span::Big, "Air");
    for (int i = 0; i < kPillCount; ++i) {
        Air &air = s_air[i];
        air.cell = plain(s_air_body);
        DialSpec spec;
        spec.size  = 120;
        spec.arc   = 12;
        spec.start = 135;
        spec.sweep = 270;
        spec.read  = Role::ReadM;
        spec.unit  = "";
        air.gauge  = new Dial(air.cell, spec);
        lv_obj_set_width(air.gauge->figure(), widest(Role::ReadM, {"8888.8"}));
        lv_obj_set_width(air.gauge->caption(), widest(Role::Small, {"\xc2\xb5g/m\xc2\xb3", "Unavailable"}));
        lv_label_set_long_mode(air.gauge->caption(), LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_margin_top(air.gauge->caption(), -space::xs, 0);
        air.caption = line(air.cell, Role::Small, Ink::Secondary, 1);
        air.word    = line(air.cell, Role::H3, Ink::Secondary, 1);
        lv_obj_set_hidden(air.cell, true);
    }
    s_air_none = empty(s_air_body, ic::air, "No readings from Home Assistant");
    lv_obj_update_layout(grid);
    place_air();
    subscribe(Topic::Home, detail::kNoView, paint_air);
}

// --- next ----------------------------------------------------------------------

struct After {
    lv_obj_t *root = nullptr, *when = nullptr, *what = nullptr;
};
lv_obj_t    *s_next = nullptr, *s_next_body = nullptr, *s_next_some = nullptr, *s_next_none = nullptr;
lv_obj_t    *s_next_dot = nullptr, *s_next_kind = nullptr, *s_next_start = nullptr, *s_next_until = nullptr;
lv_obj_t    *s_next_place = nullptr, *s_next_title = nullptr, *s_after_head = nullptr;
After        s_after[AFTER_ROWS];
std::int32_t s_next_w = 0, s_next_h = 0;

void paint_next()
{
    lv_obj_set_hidden(s_next, owner_away());  // the calendar's, away with its page
    if (!on_home() || owner_away()) {
        return;
    }
    static KIT_IN_PSRAM ical::Event ahead[AFTER_ROWS + 1];
    const int  count = ical::upcoming(ahead, AFTER_ROWS + 1);
    const auto now   = static_cast<std::int64_t>(detail::wall_now());
    lv_obj_set_hidden(s_next_some, count == 0);
    lv_obj_set_hidden(s_next_none, count > 0);
    if (count == 0) {
        return;
    }
    const ical::Event &next = ahead[0];
    char               text[ical::kSummaryMax + 32];
    const lv_color_t   ink = lv_color_hex(event_ink(next.feed));
    if (!lv_color_eq(lv_obj_get_style_bg_color(s_next_dot, LV_PART_MAIN), ink)) {
        lv_obj_set_style_bg_color(s_next_dot, ink, 0);
    }
    event_kind(next, now, text, sizeof(text));
    set_text_if(s_next_kind, text);
    event_clock(next.start, text, sizeof(text));
    set_text_if(s_next_start, text);
    char until[24];
    event_clock(next.end, text, sizeof(text));
    std::snprintf(until, sizeof(until), "\xe2\x80\x93 %.16s", text);
    set_text_if(s_next_until, until);
    event_place(next, text, sizeof(text));
    set_text_if(s_next_place, text);
    lv_obj_set_hidden(s_next_place, text[0] == '\0');
    event_course(next, text, sizeof(text));
    set_text_if(s_next_title, text);
    lv_point_t size{};
    lv_text_get_size(&size, text, fonts::h3(), 0, 0, s_next_w, LV_TEXT_FLAG_NONE);
    const std::int32_t lines = size.y > fonts::h3()->line_height ? 2 : 1;
    lv_obj_set_height(s_next_title, lines * fonts::h3()->line_height);

    for (int i = 0; i < AFTER_ROWS; ++i) {
        const bool real = i + 1 < count;
        lv_obj_set_hidden(s_after[i].root, !real);
        if (!real) {
            continue;
        }
        const auto at   = static_cast<std::time_t>(ahead[i + 1].start);
        std::tm    when{};
        localtime_r(&at, &when);
        std::strftime(text, sizeof(text), "%a %H:%M", &when);
        set_text_if(s_after[i].when, text);
        event_course(ahead[i + 1], text, sizeof(text));
        set_text_if(s_after[i].what, text);
    }
    // Only whole rows: one the tile would cut through goes, and those after it.
    lv_obj_update_layout(s_next_some);
    int shown = 0;
    for (int i = 0; i < AFTER_ROWS; ++i) {
        if (lv_obj_is_hidden(s_after[i].root)) {
            continue;
        }
        if (shown < i || lv_obj_get_y(s_after[i].root) + lv_obj_get_height(s_after[i].root) > s_next_h) {
            lv_obj_set_hidden(s_after[i].root, true);
        } else {
            ++shown;
        }
    }
    lv_obj_set_hidden(s_after_head, shown == 0);
}

void build_next(lv_obj_t *grid)
{
    lv_obj_t *body = widget(grid, Span::Big, "Next");
    s_next_body    = body;
    s_next         = lv_obj_get_parent(body);
    lv_obj_update_layout(grid);
    s_next_w = lv_obj_get_content_width(body);
    s_next_h = lv_obj_get_content_height(body);

    s_next_some = flow(body, LV_FLEX_FLOW_COLUMN, space::xs);
    lv_obj_set_size(s_next_some, s_next_w, s_next_h);
    lv_obj_t *kind = flow(s_next_some, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_flex_align(kind, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_next_dot = plain(kind);
    lv_obj_set_size(s_next_dot, DOT, DOT);
    lv_obj_set_style_radius(s_next_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_next_dot, LV_OPA_COVER, 0);
    s_next_kind = line(kind, Role::Small, Ink::Secondary, s_next_w - DOT - space::s);

    lv_obj_t *when = plain(s_next_some);
    lv_obj_set_size(when, s_next_w, fonts::read_l()->line_height);
    const std::int32_t start_w = widest(Role::ReadL, {"88:88"});
    s_next_start               = line(when, Role::ReadL, Ink::Primary, start_w);
    s_next_until               = line(when, Role::H3, Ink::Secondary, widest(Role::H3, {"\xe2\x80\x93 88:88"}));
    lv_obj_set_pos(s_next_until, start_w + space::m, baseline_y(Role::ReadL, Role::H3));
    s_next_place = line(s_next_some, Role::H3, Ink::Primary, s_next_w);
    s_next_title = text(s_next_some, Role::H3, "");
    lv_obj_set_size(s_next_title, s_next_w, fonts::h3()->line_height);
    lv_label_set_long_mode(s_next_title, LV_LABEL_LONG_MODE_DOTS);
    s_after_head = line(s_next_some, Role::Small, Ink::Secondary, s_next_w);
    lv_label_set_text(s_after_head, "After that");
    lv_obj_set_style_margin_top(s_after_head, space::m, 0);
    const std::int32_t when_w = widest(Role::Body, {"Wed 88:88"}) + space::m;
    for (After &after : s_after) {
        after.root = plain(s_next_some);
        lv_obj_set_size(after.root, s_next_w, fonts::body()->line_height);
        after.when = line(after.root, Role::Body, Ink::Secondary, when_w);
        after.what = line(after.root, Role::Body, Ink::Primary, s_next_w - when_w);
        lv_obj_set_x(after.what, when_w);
    }
    s_next_none = empty(body, ic::event, "Nothing coming up");
    lv_obj_set_size(s_next_none, s_next_w, s_next_h);
    lv_obj_set_hidden(s_next_none, true);
    widget_opens(body, CALENDAR);
}

// --- overhead ------------------------------------------------------------------

KIT_IN_PSRAM radar::Snapshot s_sky;
char         s_held[radar::kHexLen] = "";
char         s_asked[radar::kHexLen] = "";  // whose photo was asked for, this time Home is up
char         s_photo_of[radar::kHexLen] = "";  // whose photo the widget shows
std::uint32_t s_photo_stamp = 0;
Cover       *s_plane = nullptr;
lv_obj_t    *s_sky_tile = nullptr, *s_sky_some = nullptr, *s_sky_none = nullptr, *s_sky_why = nullptr;
lv_obj_t    *s_type = nullptr, *s_alt = nullptr, *s_alt_unit = nullptr;
// where it flies from and to, a line between with the plane along it; its
// callsign in their place while the route is not known
lv_obj_t    *s_from = nullptr, *s_to = nullptr, *s_track = nullptr, *s_track_fill = nullptr, *s_track_plane = nullptr;
lv_obj_t    *s_call = nullptr;
// which way it flies: a plane turned to its track, and the word
lv_obj_t    *s_heading = nullptr, *s_heading_word = nullptr;
std::int32_t s_track_x = 0, s_track_w = 0, s_track_y = 0;

const char *compass(float bearing)
{
    static const char *const POINTS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int at = static_cast<int>((bearing + 22.5f) / 45.0f);
    return POINTS[((at % 8) + 8) % 8];
}

// The closest aircraft in the air.
const radar::Aircraft *pick_overhead()
{
    const radar::Aircraft *closest = nullptr;
    for (int i = 0; i < s_sky.count; ++i) {
        const radar::Aircraft &a = s_sky.list[i];
        if (a.on_ground || a.distance_nm * KM_PER_NM > OVERHEAD_KM) {
            continue;
        }
        if (closest == nullptr || a.distance_nm < closest->distance_nm) {
            closest = &a;
        }
    }
    std::snprintf(s_held, sizeof(s_held), "%s", closest != nullptr ? closest->hex : "");
    return closest;
}

void clear_plane_photo()
{
    s_plane->clear(false);
    s_photo_of[0] = '\0';
}

// Taken as it comes or not at all, since radar decodes another over it before
// long: one there was no room for is asked for again a while on.
void show_plane_photo()
{
    const detail::RadarLookup &lookup = detail::radar_lookup();
    if (lookup.photo_stamp == s_photo_stamp || std::strcmp(lookup.photo_hex, s_held) != 0) {
        return;
    }
    s_photo_stamp = lookup.photo_stamp;
    if (lookup.photo == nullptr || lookup.photo_width <= 0 || lookup.photo_height <= 0 ||
        std::strcmp(s_photo_of, lookup.photo_hex) == 0) {
        return;
    }
    if (s_plane->show(Picture{lookup.photo, lookup.photo_width, lookup.photo_height,
                              lookup.photo_width * static_cast<std::int32_t>(sizeof(std::uint16_t))})) {
        std::snprintf(s_photo_of, sizeof(s_photo_of), "%s", lookup.photo_hex);
    } else {
        s_photo_of[0] = '\0';
    }
}

// From and to, as the radar's column has them, once the lookup for it is in.
void paint_route(const radar::Aircraft &a)
{
    const detail::RadarLookup &lookup = detail::radar_lookup();
    const radar::Details      &d      = lookup.details;
    const bool known = std::strcmp(lookup.details_hex, a.hex) == 0 && d.has_route && d.origin_code[0] != '\0' &&
                       d.dest_code[0] != '\0';
    for (lv_obj_t *part : {s_from, s_to, s_track}) {
        if (lv_obj_is_hidden(part) == known) {
            lv_obj_set_hidden(part, !known);
        }
    }
    if (lv_obj_is_hidden(s_call) != known) {
        lv_obj_set_hidden(s_call, known);
    }
    if (!known) {
        char call[48];
        if (std::strcmp(lookup.details_hex, a.hex) == 0 && d.airline[0] != '\0') {
            std::snprintf(call, sizeof(call), "%s, %s", a.flight[0] != '\0' ? a.flight : a.hex, d.airline);
        } else {
            std::snprintf(call, sizeof(call), "%s", a.flight[0] != '\0' ? a.flight : a.hex);
        }
        set_text_if(s_call, call);
        lv_obj_set_hidden(s_track_plane, true);
        return;
    }
    set_text_if(s_from, d.origin_code);
    set_text_if(s_to, d.dest_code);
    float share = 0.0f, left_km = 0.0f;
    const bool along = radar::route_progress(d, a.lat, a.lon, share, left_km);
    const std::int32_t at = along ? static_cast<std::int32_t>(std::lround(share * s_track_w)) : 0;
    if (lv_obj_get_width(s_track_fill) != at) {
        lv_obj_set_width(s_track_fill, at);
    }
    lv_obj_set_hidden(s_track_plane, !along);
    if (along) {
        lv_obj_set_pos(s_track_plane, s_track_x + at - icons::plane_icon.header.w / 2,
                       s_track_y - static_cast<std::int32_t>(icons::plane_icon.header.h) / 2);
    }
}

void paint_overhead()
{
    lv_obj_set_hidden(s_sky_tile, owner_away());
    if (!on_home() || owner_away()) {
        return;
    }
    radar::snapshot(s_sky);
    const radar::Aircraft *a   = s_sky.age_s >= 0 ? pick_overhead() : nullptr;
    const char            *why = s_sky.age_s < 0 ? "No radar yet" : !s_sky.ok ? "The radar cannot be reached" : "Nothing overhead";
    lv_obj_set_hidden(s_sky_some, a == nullptr);
    lv_obj_set_hidden(s_sky_none, a != nullptr);
    if (a == nullptr) {
        set_text_if(s_sky_why, why);
        return;
    }
    char text[96];
    sentence_case(a->desc[0] != '\0' ? a->desc : a->type[0] != '\0' ? a->type : "Aircraft", text, sizeof(text));
    set_text_if(s_type, text);
    paint_route(*a);
    const int ft = a->altitude_ft;
    if (ft >= 1000) {
        std::snprintf(text, sizeof(text), "%d,%03d", ft / 1000, ft % 1000);
    } else {
        std::snprintf(text, sizeof(text), "%d", ft);
    }
    set_text_if(s_alt, text);
    lv_obj_set_x(s_alt_unit, lv_obj_get_x(s_alt) + widest(Role::ReadS, {text}) + space::xs);
    // turned in steps of five degrees, so a plane holding its course draws once
    const std::int32_t turn = static_cast<std::int32_t>(std::lround(a->track_deg / 5.0f)) * 50 % 3600;
    if (lv_image_get_rotation(s_heading) != turn) {
        lv_image_set_rotation(s_heading, turn);
    }
    std::snprintf(text, sizeof(text), "%s, %d km", compass(a->track_deg),
                  static_cast<int>(a->distance_nm * KM_PER_NM + 0.5f));
    set_text_if(s_heading_word, text);

    if (std::strcmp(s_photo_of, a->hex) != 0 && s_plane->has_picture()) {
        clear_plane_photo();  // another's photo, until this one's comes
    }
    // Its photo: asked for once each time Home is up, as the radar page asks
    // for its own aircraft's while it is, and again while it has not shown.
    // At most one lookup a while: the closest changes as they pass, and the
    // lookups share their services' limits with the radar page's.
    static std::uint32_t s_asked_ms = 0;
    const bool another = std::strcmp(s_asked, a->hex) != 0;
    const bool missing = std::strcmp(s_photo_of, a->hex) != 0 && !radar::known_without_photo(a->hex);
    if ((another || missing) && (s_asked[0] == '\0' || lv_tick_elaps(s_asked_ms) >= LOOKUP_EVERY_MS)) {
        s_asked_ms = lv_tick_get();
        std::snprintf(s_asked, sizeof(s_asked), "%s", a->hex);
        if (s_plane->has_picture()) {
            clear_plane_photo();
        }
        // only what comes of this: one in hand is handed over again at once
        s_photo_stamp = detail::radar_lookup().photo_stamp;
        if (detail::s_handlers.details != nullptr) {
            detail::s_handlers.details(a->hex, a->flight);
        }
    }
    show_plane_photo();
}

void paint_sky_inks(void *)
{
    lv_obj_set_style_bg_color(s_track, colour(Ink::Primary), 0);
    lv_obj_set_style_bg_opa(s_track, colours().ink3_opa, 0);
    lv_obj_set_style_bg_color(s_track_fill, colour(Ink::Accent), 0);
    lv_obj_set_style_image_recolor(s_track_plane, colour(Ink::Accent), 0);
    lv_obj_set_style_image_recolor(s_heading, colour(Ink::Primary), 0);
}

void build_overhead(lv_obj_t *grid)
{
    lv_obj_t *body = widget(grid, Span::Wide, "Overhead");
    s_sky_tile     = lv_obj_get_parent(body);
    lv_obj_update_layout(grid);
    const std::int32_t w = lv_obj_get_content_width(body), h = lv_obj_get_content_height(body);

    s_sky_some = plain(body);
    lv_obj_set_size(s_sky_some, w, h);
    // its photo at the left, as tall as the widget, and what it is beside it
    const std::int32_t photo_w = h * 16 / 9;
    s_plane = new Cover(s_sky_some, photo_w, h, Shape::Rounded, radius::inner, ic::flight);
    lv_obj_set_pos(s_plane->obj(), 0, 0);
    const std::int32_t x0 = photo_w + space::l, text_w = w - x0;
    // what it is, then where it flies from and to, then how high and which way
    s_type = line(s_sky_some, Role::H3, Ink::Primary, text_w);
    lv_obj_set_pos(s_type, x0, 0);
    const std::int32_t route_y = fonts::h3()->line_height + space::xs;
    const std::int32_t code_w  = widest(Role::Body, {"WWW"});
    s_from = line(s_sky_some, Role::Body, Ink::Primary, code_w);
    s_to   = line(s_sky_some, Role::Body, Ink::Primary, code_w);
    lv_obj_set_style_text_align(s_to, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_from, x0, route_y);
    lv_obj_set_pos(s_to, x0 + text_w - code_w, route_y);
    s_track_x = x0 + code_w + space::m;
    s_track_w = text_w - 2 * (code_w + space::m);
    s_track_y = route_y + fonts::body()->line_height / 2;
    s_track   = plain(s_sky_some);
    lv_obj_set_pos(s_track, s_track_x, s_track_y - 1);
    lv_obj_set_size(s_track, s_track_w, 2);
    lv_obj_set_style_radius(s_track, 1, 0);
    s_track_fill = plain(s_track);
    lv_obj_set_size(s_track_fill, 0, 2);
    lv_obj_set_style_radius(s_track_fill, 1, 0);
    lv_obj_set_style_bg_opa(s_track_fill, LV_OPA_COVER, 0);
    s_track_plane = lv_image_create(s_sky_some);
    lv_image_set_src(s_track_plane, &icons::plane_icon);
    lv_image_set_rotation(s_track_plane, 900);  // along the line, to the right
    lv_obj_set_style_image_recolor_opa(s_track_plane, LV_OPA_COVER, 0);
    lv_obj_set_hidden(s_track_plane, true);
    s_call = line(s_sky_some, Role::Small, Ink::Secondary, text_w);
    lv_obj_set_pos(s_call, x0, route_y + (fonts::body()->line_height - fonts::small()->line_height) / 2);

    // at the foot, as the desk's and the focus timer's figures are
    const std::int32_t foot   = h - fonts::read_s()->line_height;
    const std::int32_t unit_w = widest(Role::Body, {"ft"});
    s_alt      = line(s_sky_some, Role::ReadS, Ink::Primary, widest(Role::ReadS, {"88,888"}));
    s_alt_unit = line(s_sky_some, Role::Body, Ink::Secondary, unit_w);
    lv_label_set_text(s_alt_unit, "ft");
    lv_obj_set_pos(s_alt, x0, foot);
    lv_obj_set_pos(s_alt_unit, x0, foot + baseline_y(Role::ReadS, Role::Body));
    const std::int32_t word_w = widest(Role::Small, {"NW, 888 km"});
    s_heading_word = line(s_sky_some, Role::Small, Ink::Secondary, word_w);
    const std::int32_t word_y = foot + baseline_y(Role::ReadS, Role::Small);
    lv_obj_set_pos(s_heading_word, x0 + text_w - word_w, word_y);
    s_heading = lv_image_create(s_sky_some);
    lv_image_set_src(s_heading, &icons::plane_icon);
    lv_obj_set_style_image_recolor_opa(s_heading, LV_OPA_COVER, 0);
    paint_sky_inks(nullptr);
    on_restyle(s_sky_some, paint_sky_inks, nullptr);
    lv_obj_set_pos(s_heading, x0 + text_w - word_w - space::s - static_cast<std::int32_t>(icons::plane_icon.header.w),
                   word_y + (fonts::small()->line_height - static_cast<std::int32_t>(icons::plane_icon.header.h)) / 2);

    s_sky_none = empty(body, ic::flight, "");
    lv_obj_set_size(s_sky_none, w, h);
    lv_obj_set_style_pad_ver(s_sky_none, 0, 0);
    s_sky_why = lv_obj_get_child(s_sky_none, -1);
    lv_obj_set_hidden(s_sky_none, true);
    // Before widget_opens' own, so the page opens on this one rather than the most notable.
    lv_obj_add_event_cb(
        s_sky_tile, [](lv_event_t *) { radar_opens_on(s_held); }, LV_EVENT_CLICKED, nullptr);
    widget_opens(body, RADAR);
}

// --- desk ------------------------------------------------------------------------

lv_obj_t *s_desk = nullptr, *s_desk_unit = nullptr, *s_desk_at = nullptr;

void paint_desk()
{
    if (!on_home()) {
        return;
    }
    const detail::DeskState &desk = detail::desk_state();
    char                     text[48] = "--";
    if (desk.height_mm >= 0) {
        std::snprintf(text, sizeof(text), "%d.%d", desk.height_mm / 10, desk.height_mm % 10);
    }
    set_text_if(s_desk, text);
    lv_obj_set_x(s_desk_unit, widest(Role::ReadL, {text}) + space::xs);
    ink_if(s_desk, desk.available ? Ink::Primary : Ink::Tertiary);

    text[0] = '\0';
    if (!desk.available) {
        std::snprintf(text, sizeof(text), "Not answering");
    } else if (desk.moving != 0) {
        std::snprintf(text, sizeof(text), "%s", desk.moving > 0 ? "Going up" : "Going down");
    } else {
        for (int i = 0; i < kPresetCount; ++i) {
            if (desk.preset_active[i]) {
                std::snprintf(text, sizeof(text), "At %s", detail::PRESET_NAMES[i]);
                break;
            }
        }
    }
    set_text_if(s_desk_at, text);
}

// A big figure at the foot of a small widget, and a line under it.
std::int32_t figure_y(std::int32_t h) { return h - fonts::small()->line_height - fonts::read_l()->line_height; }

lv_obj_t *figure_at_foot(lv_obj_t *body, std::int32_t w, std::int32_t h, lv_obj_t *&detail_line)
{
    const std::int32_t detail_h = fonts::small()->line_height;
    lv_obj_t          *figure   = line(body, Role::ReadL, Ink::Primary, widest(Role::ReadL, {"888.8", "88:88"}));
    lv_obj_set_y(figure, figure_y(h));
    detail_line = line(body, Role::Small, Ink::Secondary, w);
    lv_obj_set_y(detail_line, h - detail_h);
    return figure;
}

lv_obj_t *s_desk_body = nullptr;

void place_desk()
{
    const std::int32_t w = lv_obj_get_content_width(s_desk_body), h = lv_obj_get_content_height(s_desk_body);
    lv_obj_set_y(s_desk, figure_y(h));
    lv_obj_set_y(s_desk_unit, figure_y(h) + baseline_y(Role::ReadL, Role::Body));
    lv_obj_set_y(s_desk_at, h - fonts::small()->line_height);
    lv_obj_set_width(s_desk_at, w);
}

void build_desk(lv_obj_t *grid)
{
    s_desk_body = widget(grid, Span::One, "Desk");
    lv_obj_update_layout(grid);
    const std::int32_t w = lv_obj_get_content_width(s_desk_body), h = lv_obj_get_content_height(s_desk_body);
    s_desk      = figure_at_foot(s_desk_body, w, h, s_desk_at);
    s_desk_unit = line(s_desk_body, Role::Body, Ink::Secondary, widest(Role::Body, {"cm"}));
    lv_label_set_text(s_desk_unit, "cm");
    place_desk();
    subscribe(Topic::Desk, detail::kNoView, paint_desk);
}

// --- focus ---------------------------------------------------------------------

lv_obj_t *s_focus_tile = nullptr, *s_focus = nullptr, *s_focus_ready = nullptr, *s_focus_state = nullptr;

void paint_focus()
{
    lv_obj_set_hidden(s_focus_tile, owner_away());  // as the dock's timer is
    if (!on_home()) {
        return;
    }
    const Focus &focus   = detail::focus_state();
    const bool   waiting = detail::focus_waiting(focus);
    char         text[48];
    detail::focus_clock_text(focus, text, sizeof(text));
    lv_obj_set_hidden(s_focus, waiting);
    lv_obj_set_hidden(s_focus_ready, !waiting);
    if (!waiting) {
        set_text_if(s_focus, text);
    }
    const bool idle = detail::focus_idle(focus);
    ink_if(s_focus, idle ? Ink::Tertiary : detail::focus_resting(focus) ? Ink::Green : Ink::Primary);

    const char *part = detail::focus_resting(focus) ? "Break" : "Focus";
    if (idle) {
        std::snprintf(text, sizeof(text), "Not running");
    } else if (waiting) {
        std::snprintf(text, sizeof(text), "%s, round %d of %d", part, focus.round, focus.rounds);
    } else if (!focus.running) {
        std::snprintf(text, sizeof(text), "%s, paused", part);
    } else {
        const std::time_t ends = detail::wall_now() + detail::whole_seconds_up(detail::focus_left_ms(focus));
        std::tm           at{};
        localtime_r(&ends, &at);
        std::snprintf(text, sizeof(text), "%s, until %02d:%02d", part, at.tm_hour, at.tm_min);
    }
    set_text_if(s_focus_state, text);
}

void build_focus(lv_obj_t *grid)
{
    lv_obj_t *body = widget(grid, Span::One, "Focus");
    s_focus_tile   = lv_obj_get_parent(body);
    lv_obj_update_layout(grid);
    const std::int32_t w = lv_obj_get_content_width(body), h = lv_obj_get_content_height(body);
    s_focus       = figure_at_foot(body, w, h, s_focus_state);
    s_focus_ready = line(body, Role::H1, Ink::Accent, w);
    lv_label_set_text(s_focus_ready, "Ready");
    lv_obj_set_y(s_focus_ready, figure_y(h) + baseline_y(Role::ReadL, Role::H3) -
                                    (fonts::h1()->line_height - fonts::h3()->line_height));
    lv_obj_set_hidden(s_focus_ready, true);
    subscribe(Topic::Focus, detail::kNoView, paint_focus);
    subscribe(Topic::Second, detail::kNoView, [] {
        if (detail::focus_state().running) {
            paint_focus();
        }
    });
}

// Away, with the calendar's, the radar's and the timer's widgets gone, the
// air and the desk widen to fill the rows rather than leave holes where those were.
void lay_out()
{
    static int s_away = -1;
    const bool away   = owner_away();
    if (std::exchange(s_away, away ? 1 : 0) == (away ? 1 : 0)) {
        return;
    }
    widget_place(s_air_body, 0, 0, away ? 4 : 2, 2);
    widget_place(s_desk_body, away ? 0 : 2, 2, away ? 4 : 1, 1);
    place_air();
    place_desk();
}

void paint_all()
{
    paint_air();
    paint_next();
    paint_overhead();
    paint_desk();
    paint_focus();
}
}  // namespace

void build_home(lv_obj_t *page)
{
    lv_obj_t *grid = widget_grid(page);
    build_air(grid);
    build_next(grid);
    build_overhead(grid);
    build_desk(grid);
    build_focus(grid);

    subscribe(Topic::Calendar, detail::kNoView, paint_next);
    subscribe(Topic::Radar, detail::kNoView, paint_overhead);
    for (Topic away : {Topic::Status, Topic::Settings}) {
        subscribe(away, detail::kNoView, [] {
            lay_out();
            paint_next();
            paint_overhead();
            paint_focus();
        });
    }
    subscribe(Topic::Page, detail::kNoView, [] {
        if (!on_home()) {
            s_asked[0] = '\0';  // the radar page asks for its own aircraft's meanwhile
        }
        paint_all();
    });
    subscribe(Topic::Lookup, detail::kNoView, paint_overhead);  // its route and its photo, as they come
    lv_timer_create([](lv_timer_t *) { paint_next(); }, REFRESH_MS, nullptr);
}

}  // namespace ui::kitfront
