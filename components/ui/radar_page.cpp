#include "radar_page.h"

#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {
namespace {

constexpr float DEG    = 3.14159265f / 180.0f;
constexpr int   MARK   = 24;  // room outside the outer ring for the compass letters
constexpr int   SPOKES = 12;
constexpr int   RINGS  = 4;
constexpr int   WING   = 6;
constexpr int   NOSE   = 9;
constexpr int   TAIL   = 7;
constexpr int   BOX    = 20;  // local frame each triangle is drawn in
constexpr int   DETAIL_ROWS = 6;
constexpr float KM_PER_NM   = 1.852f;

struct Blip {
    lv_obj_t          *shape;
    lv_obj_t          *label;
    lv_point_precise_t points[4];
    char               hex[radar::kHexLen];
};

struct Row {
    lv_obj_t *label;
    lv_obj_t *value;
};

lv_obj_t *s_scope  = nullptr;
lv_obj_t *s_marker = nullptr;
lv_obj_t *s_range  = nullptr;

Blip               s_blips[radar::kMaxAircraft] = {};
lv_point_precise_t s_spokes[SPOKES][2]          = {};

lv_obj_t *s_title    = nullptr;
lv_obj_t *s_subtitle = nullptr;
lv_obj_t *s_desc     = nullptr;
lv_obj_t *s_route    = nullptr;
lv_obj_t *s_cities   = nullptr;
lv_obj_t *s_operator = nullptr;
lv_obj_t *s_summary  = nullptr;
lv_obj_t *s_photo    = nullptr;
lv_image_dsc_t s_photo_dsc = {};
Row       s_rows[DETAIL_ROWS] = {};

// Held against the selection so a lookup that comes back after the tap has
// moved on is thrown away rather than shown against the wrong aircraft.
radar::Details s_details      = {};
char           s_details_hex[radar::kHexLen] = {};

// Held so the panel can be redrawn when the selection changes rather than only
// when a reading lands.
radar::Snapshot s_last     = {};
char            s_chosen[radar::kHexLen] = {};

std::int32_t s_centre = 0;
std::int32_t s_radius = 0;

struct Plot {
    const radar::Aircraft *aircraft;
    float                  east_nm;
    float                  north_nm;
    std::int32_t           x;
    std::int32_t           y;
};

Plot s_plots[radar::kMaxAircraft];
int  s_shown = 0;

std::uint32_t altitude_ink(int feet)
{
    if (feet < 0) {
        return theme::secondary;
    }
    if (feet < 10000) {
        return theme::orange;
    }
    return feet < 25000 ? theme::text : theme::green;
}

void quiet(lv_obj_t *obj)
{
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
}

void ring(lv_obj_t *parent, std::int32_t radius, lv_opa_t opa)
{
    lv_obj_t *circle = lv_obj_create(parent);
    lv_obj_set_size(circle, radius * 2, radius * 2);
    lv_obj_set_pos(circle, s_centre - radius, s_centre - radius);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(circle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(circle, lv_color_hex(theme::green), 0);
    lv_obj_set_style_border_opa(circle, opa, 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    quiet(circle);
}

void compass(lv_obj_t *parent, const char *text, std::int32_t dx, std::int32_t dy)
{
    lv_obj_t *label = theme::make_label(parent, text, theme::secondary, fonts::size_16());
    lv_obj_set_pos(label, s_centre + dx - 6, s_centre + dy - 9);
    quiet(label);
}

// Bearing lines and labelled rings: without them a dot is somewhere vaguely
// over there, and with them it is eleven miles out to the north east.
void build_chart(lv_obj_t *scope, int range_km)
{
    for (int i = 0; i < SPOKES; ++i) {
        const float angle = static_cast<float>(i) * 360.0f / SPOKES * DEG;
        const auto  inner = static_cast<float>(s_radius) / RINGS;
        s_spokes[i][0]    = {static_cast<std::int32_t>(s_centre + std::sin(angle) * inner),
                             static_cast<std::int32_t>(s_centre - std::cos(angle) * inner)};
        s_spokes[i][1]    = {static_cast<std::int32_t>(s_centre + std::sin(angle) * s_radius),
                             static_cast<std::int32_t>(s_centre - std::cos(angle) * s_radius)};

        lv_obj_t *spoke = lv_line_create(scope);
        lv_line_set_points(spoke, s_spokes[i], 2);
        lv_obj_set_pos(spoke, 0, 0);
        lv_obj_set_style_line_width(spoke, 1, 0);
        lv_obj_set_style_line_color(spoke, lv_color_hex(theme::green), 0);
        lv_obj_set_style_line_opa(spoke, LV_OPA_20, 0);
        quiet(spoke);
    }

    for (int i = 1; i <= RINGS; ++i) {
        const std::int32_t radius = s_radius * i / RINGS;
        ring(scope, radius, i == RINGS ? LV_OPA_50 : LV_OPA_20);

        char text[8];
        std::snprintf(text, sizeof(text), "%d", range_km * i / RINGS);
        lv_obj_t *label = theme::make_label(scope, text, theme::secondary, fonts::size_16());
        lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
        lv_obj_set_pos(label, s_centre + 7, s_centre - radius - 9);
        quiet(label);
    }
}

// Aiming at a twenty pixel triangle on a touchscreen is a game nobody wants to
// play. A tap anywhere on the scope takes the nearest aircraft within a tenth
// of the scope's width, and tapping open sky lets go of the one that was held.
void scope_clicked(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == nullptr) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t area;
    lv_obj_get_coords(s_scope, &area);
    const std::int32_t x = point.x - area.x1;
    const std::int32_t y = point.y - area.y1;

    const std::int32_t reach   = lv_obj_get_width(s_scope) / 10;
    std::int32_t       nearest = reach * reach;
    int                best    = -1;
    for (int i = 0; i < s_shown; ++i) {
        const std::int32_t dx = x - s_plots[i].x;
        const std::int32_t dy = y - s_plots[i].y;
        const std::int32_t d2 = dx * dx + dy * dy;
        if (d2 <= nearest) {
            nearest = d2;
            best    = i;
        }
    }

    if (best < 0) {
        s_chosen[0] = '\0';
    } else {
        std::memcpy(s_chosen, s_plots[best].aircraft->hex, sizeof(s_chosen));
        s_chosen[sizeof(s_chosen) - 1] = '\0';
        if (std::strcmp(s_details_hex, s_chosen) != 0) {
            s_details        = radar::Details{};
            s_details_hex[0] = '\0';
            // Whatever is on screen belongs to the aircraft just let go of.
            lv_obj_set_hidden(s_photo, true);
            radar::request_details(s_chosen, s_plots[best].aircraft->flight);
        }
    }
    show_radar(s_last);
}

void build_scope(lv_obj_t *parent, std::int32_t side, int range_km)
{
    s_centre = side / 2;
    s_radius = side / 2 - MARK;

    lv_obj_t *scope = lv_obj_create(parent);
    lv_obj_set_pos(scope, 0, 0);
    lv_obj_set_size(scope, side, side);
    theme::style_panel(scope, theme::background, side / 2);
    lv_obj_set_scrollable(scope, false);
    lv_obj_add_event_cb(scope, scope_clicked, LV_EVENT_CLICKED, nullptr);

    build_chart(scope, range_km);

    compass(scope, "N", 0, -s_radius - 6);
    compass(scope, "S", 0, s_radius + 6);
    compass(scope, "E", s_radius + 8, 0);
    compass(scope, "W", -s_radius - 8, 0);

    lv_obj_t *home = lv_obj_create(scope);
    lv_obj_set_size(home, 8, 8);
    lv_obj_set_pos(home, s_centre - 4, s_centre - 4);
    theme::style_panel(home, theme::orange, 4);
    quiet(home);

    s_marker = lv_obj_create(scope);
    lv_obj_set_size(s_marker, 34, 34);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_marker, lv_color_hex(theme::orange), 0);
    lv_obj_set_style_border_width(s_marker, 2, 0);
    lv_obj_set_style_pad_all(s_marker, 0, 0);
    lv_obj_set_hidden(s_marker, true);
    quiet(s_marker);

    // A fixed pool: rebuilding these on every reading would churn the heap
    // every ten seconds for the whole uptime.
    for (int i = 0; i < radar::kMaxAircraft; ++i) {
        Blip &blip = s_blips[i];
        blip.shape = lv_line_create(scope);
        lv_obj_set_style_line_width(blip.shape, 2, 0);
        lv_obj_set_style_line_rounded(blip.shape, true, 0);
        lv_obj_set_hidden(blip.shape, true);
        quiet(blip.shape);

        blip.label = theme::make_label(scope, "", theme::secondary, fonts::size_16());
        lv_obj_set_hidden(blip.label, true);
        quiet(blip.label);
    }

    s_scope = scope;
}

void build_panel(lv_obj_t *parent, std::int32_t x, std::int32_t width, std::int32_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, 0);
    lv_obj_set_size(panel, width, height);
    theme::style_panel(panel, theme::panel_light, 16);
    lv_obj_set_style_pad_all(panel, 20, 0);
    quiet(panel);

    s_title = theme::make_label(panel, "Radar", theme::orange, fonts::size_32());
    lv_obj_set_pos(s_title, 0, 0);
    lv_obj_set_width(s_title, width - 40);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_title, fonts::size_32()->line_height);

    s_subtitle = theme::make_label(panel, "", theme::text, fonts::size_20());
    lv_obj_set_pos(s_subtitle, 0, 40);
    lv_obj_set_width(s_subtitle, width - 40);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_subtitle, fonts::size_20()->line_height);

    s_desc = theme::make_label(panel, "", theme::secondary, fonts::size_16());
    lv_obj_set_pos(s_desc, 0, 66);
    lv_obj_set_width(s_desc, width - 40);
    lv_label_set_long_mode(s_desc, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_desc, fonts::size_16()->line_height);

    s_route = theme::make_label(panel, "", theme::orange, fonts::size_28());
    lv_obj_set_pos(s_route, 0, 94);
    lv_obj_set_width(s_route, width - 40);
    lv_label_set_long_mode(s_route, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_route, fonts::size_28()->line_height);

    s_cities = theme::make_label(panel, "", theme::text, fonts::size_16());
    lv_obj_set_pos(s_cities, 0, 128);
    lv_obj_set_width(s_cities, width - 40);
    lv_label_set_long_mode(s_cities, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_cities, fonts::size_16()->line_height);

    s_operator = theme::make_label(panel, "", theme::secondary, fonts::size_16());
    lv_obj_set_pos(s_operator, 0, 150);
    lv_obj_set_width(s_operator, width - 40);
    lv_label_set_long_mode(s_operator, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_operator, fonts::size_16()->line_height);

    static const char *NAMES[DETAIL_ROWS] = {"Altitude", "Ground speed", "Track",
                                             "Distance", "Bearing",      "Squawk"};
    for (int i = 0; i < DETAIL_ROWS; ++i) {
        lv_obj_t *row = lv_obj_create(panel);
        lv_obj_set_pos(row, 0, 182 + i * 38);
        lv_obj_set_size(row, width - 40, 34);
        theme::style_panel(row, theme::panel, 8);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        quiet(row);

        s_rows[i].label = theme::make_label(row, NAMES[i], theme::secondary, fonts::size_16());
        s_rows[i].value = theme::make_label(row, "--", theme::text, fonts::size_16());
        lv_obj_set_flex_grow(s_rows[i].value, 1);
        lv_obj_set_height(s_rows[i].value, fonts::size_16()->line_height);
        lv_obj_set_style_text_align(s_rows[i].value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(s_rows[i].value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_hidden(row, true);
    }

    // Sits under the readings, so it takes whatever room is left rather than a
    // fixed slot: a photo that never arrives should not leave a hole.
    s_photo = lv_image_create(panel);
    lv_obj_set_pos(s_photo, 0, 182 + DETAIL_ROWS * 38 + 10);
    lv_obj_set_hidden(s_photo, true);
    lv_obj_set_clickable(s_photo, false);

    // What the colours on the scope mean, since nothing else says so.
    static const struct {
        std::uint32_t colour;
        const char   *text;
    } LEGEND[] = {
        {theme::green, "above 25 000 ft"},
        {theme::text, "10 000 to 25 000 ft"},
        {theme::orange, "below 10 000 ft"},
    };
    const int legend_count = static_cast<int>(sizeof(LEGEND) / sizeof(LEGEND[0]));
    for (int i = 0; i < legend_count; ++i) {
        const std::int32_t y = -(legend_count - 1 - i) * 22;

        lv_obj_t *swatch = lv_obj_create(panel);
        lv_obj_set_size(swatch, 14, 4);
        theme::style_panel(swatch, LEGEND[i].colour, 2);
        lv_obj_align(swatch, LV_ALIGN_BOTTOM_LEFT, 0, y - 7);
        quiet(swatch);

        lv_obj_t *label = theme::make_label(panel, LEGEND[i].text, theme::secondary,
                                            fonts::size_16());
        lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 22, y);
        quiet(label);
    }

    // Only ever says anything when something is wrong.
    s_summary = theme::make_label(panel, "", theme::amber, fonts::size_16());
    lv_obj_set_width(s_summary, width - 40);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_summary, fonts::size_16()->line_height);
    lv_obj_align(s_summary, LV_ALIGN_BOTTOM_LEFT, 0, -(legend_count * 22) - 6);

    s_range = theme::make_label(panel, "", theme::secondary, fonts::size_16());
    lv_obj_align(s_range, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

void draw_blip(Blip &blip, Plot &plot, float range_km)
{
    const float scale = static_cast<float>(s_radius) / range_km;
    plot.x = static_cast<std::int32_t>(std::lround(s_centre + plot.east_nm * scale));
    plot.y = static_cast<std::int32_t>(std::lround(s_centre - plot.north_nm * scale));
    const std::int32_t out_x = plot.x;
    const std::int32_t out_y = plot.y;

    // Heading is clockwise from north and the screen's y runs downwards, which
    // is what puts the minus on the sine of y rather than of x.
    const float track = plot.aircraft->track_deg < 0.0f ? 0.0f : plot.aircraft->track_deg;
    const float sin_t = std::sin(track * DEG);
    const float cos_t = std::cos(track * DEG);

    const float shape[3][2] = {{0.0f, -NOSE}, {WING, TAIL}, {-WING, TAIL}};
    for (int i = 0; i < 3; ++i) {
        const float px = shape[i][0] * cos_t - shape[i][1] * sin_t;
        const float py = shape[i][0] * sin_t + shape[i][1] * cos_t;
        blip.points[i] = {static_cast<std::int32_t>(std::lround(px)) + BOX / 2,
                          static_cast<std::int32_t>(std::lround(py)) + BOX / 2};
    }
    blip.points[3] = blip.points[0];

    lv_line_set_points(blip.shape, blip.points, 4);
    lv_obj_set_pos(blip.shape, out_x - BOX / 2, out_y - BOX / 2);
    lv_obj_set_style_line_color(blip.shape, lv_color_hex(altitude_ink(plot.aircraft->altitude_ft)),
                                0);
    lv_obj_set_hidden(blip.shape, false);

    const char *name =
        plot.aircraft->flight[0] != '\0' ? plot.aircraft->flight : plot.aircraft->hex;
    theme::set_text(blip.label, name);
    lv_obj_set_pos(blip.label, out_x + 11, out_y - 8);
    lv_obj_set_hidden(blip.label, false);

    std::memcpy(blip.hex, plot.aircraft->hex, sizeof(blip.hex));
    blip.hex[sizeof(blip.hex) - 1] = '\0';
}

void show_summary(const radar::Snapshot &snapshot)
{
    theme::set_text(s_title, "Radar");
    theme::set_text(s_subtitle, s_shown > 0 ? "Tap an aircraft" : "");
    theme::set_text(s_desc, "");
    theme::set_text(s_route, "");
    theme::set_text(s_cities, "");
    theme::set_text(s_operator, "");
    lv_obj_set_hidden(s_photo, true);
    for (Row &row : s_rows) {
        lv_obj_set_hidden(lv_obj_get_parent(row.label), true);
    }

    theme::set_text(s_summary, snapshot.age_s < 0   ? "waiting for a fix"
                               : !snapshot.ok      ? "feed unreachable"
                                                   : "");
}

void show_selected(const radar::Aircraft &aircraft)
{
    char text[80];

    theme::set_text(s_title, aircraft.flight[0] != '\0' ? aircraft.flight : aircraft.hex);

    if (aircraft.reg[0] != '\0' && aircraft.type[0] != '\0') {
        std::snprintf(text, sizeof(text), "%s  %s", aircraft.reg, aircraft.type);
    } else {
        std::snprintf(text, sizeof(text), "%s",
                      aircraft.reg[0] != '\0'
                          ? aircraft.reg
                          : (aircraft.type[0] != '\0' ? aircraft.type : aircraft.hex));
    }
    theme::set_text(s_subtitle, text);
    theme::set_text(s_desc, aircraft.desc);

    // Only what came back for this aircraft; the lookup is one flight behind
    // whenever a tap lands while the last one is still in the air.
    const bool mine = std::strcmp(s_details_hex, aircraft.hex) == 0;
    if (mine && s_details.has_route) {
        std::snprintf(text, sizeof(text), "%s  " LV_SYMBOL_RIGHT "  %s",
                      s_details.origin_code[0] != '\0' ? s_details.origin_code : "?",
                      s_details.dest_code[0] != '\0' ? s_details.dest_code : "?");
        theme::set_text(s_route, text);

        std::snprintf(text, sizeof(text), "%s to %s", s_details.origin_city, s_details.dest_city);
        theme::set_text(s_cities, text);
    } else {
        theme::set_text(s_route, mine ? "No route known" : "");
        theme::set_text(s_cities, "");
    }

    if (mine && s_details.airline[0] != '\0') {
        theme::set_text(s_operator, s_details.airline);
    } else if (mine && s_details.owner[0] != '\0') {
        theme::set_text(s_operator, s_details.owner);
    } else {
        theme::set_text(s_operator, "");
    }

    for (Row &row : s_rows) {
        lv_obj_set_hidden(lv_obj_get_parent(row.label), false);
    }

    if (aircraft.altitude_ft >= 0) {
        const char *trend = aircraft.vertical_fpm > 200    ? " " LV_SYMBOL_UP
                            : aircraft.vertical_fpm < -200 ? " " LV_SYMBOL_DOWN
                                                           : "";
        std::snprintf(text, sizeof(text), "%d ft%s", aircraft.altitude_ft, trend);
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    theme::set_text(s_rows[0].value, text);
    theme::set_text_color(s_rows[0].value, altitude_ink(aircraft.altitude_ft));

    std::snprintf(text, sizeof(text), "%.0f kt", static_cast<double>(aircraft.speed_kt));
    theme::set_text(s_rows[1].value, text);

    if (aircraft.track_deg >= 0.0f) {
        std::snprintf(text, sizeof(text), "%.0f°", static_cast<double>(aircraft.track_deg));
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    theme::set_text(s_rows[2].value, text);

    std::snprintf(text, sizeof(text), "%.1f km",
                  static_cast<double>(aircraft.distance_nm * KM_PER_NM));
    theme::set_text(s_rows[3].value, text);

    std::snprintf(text, sizeof(text), "%.0f°", static_cast<double>(aircraft.bearing_deg));
    theme::set_text(s_rows[4].value, text);

    if (aircraft.squawk >= 0) {
        std::snprintf(text, sizeof(text), "%04d", aircraft.squawk);
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    theme::set_text(s_rows[5].value, text);
}

}  // namespace

void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    const std::int32_t side = std::min(width - 320, height);
    build_scope(page, side, 80);
    build_panel(page, side + 16, width - side - 16, height);
    show_radar(s_last);
}

void show_radar(const radar::Snapshot &snapshot)
{
    if (s_scope == nullptr) {
        return;
    }
    if (&snapshot != &s_last) {
        s_last = snapshot;
    }

    char text[24];
    std::snprintf(text, sizeof(text), "%d km", s_last.range_km > 0 ? s_last.range_km : 80);
    theme::set_text(s_range, text);

    s_shown = 0;
    for (int i = 0; i < s_last.count && s_shown < radar::kMaxAircraft; ++i) {
        const radar::Aircraft &aircraft = s_last.list[i];
        const float distance_km = aircraft.distance_nm * KM_PER_NM;
        if (aircraft.on_ground || distance_km > static_cast<float>(s_last.range_km)) {
            continue;
        }
        s_plots[s_shown].aircraft = &s_last.list[i];
        s_plots[s_shown].east_nm  = distance_km * std::sin(aircraft.bearing_deg * DEG);
        s_plots[s_shown].north_nm = distance_km * std::cos(aircraft.bearing_deg * DEG);
        ++s_shown;
    }

    std::sort(s_plots, s_plots + s_shown, [](const Plot &a, const Plot &b) {
        return a.aircraft->distance_nm < b.aircraft->distance_nm;
    });

    const radar::Aircraft *chosen = nullptr;
    for (int i = 0; i < radar::kMaxAircraft; ++i) {
        if (i < s_shown) {
            draw_blip(s_blips[i], s_plots[i], static_cast<float>(s_last.range_km));
            if (s_chosen[0] != '\0' && std::strcmp(s_chosen, s_plots[i].aircraft->hex) == 0) {
                chosen = s_plots[i].aircraft;
                lv_obj_set_pos(s_marker, s_plots[i].x - 17, s_plots[i].y - 17);
                lv_obj_set_hidden(s_marker, false);
            }
        } else {
            lv_obj_set_hidden(s_blips[i].shape, true);
            lv_obj_set_hidden(s_blips[i].label, true);
            s_blips[i].hex[0] = '\0';
        }
    }

    if (chosen != nullptr) {
        show_selected(*chosen);
        theme::set_text(s_summary, s_last.ok ? "" : "feed unreachable");
    } else {
        // Whatever was selected has left the scope.
        s_chosen[0] = '\0';
        lv_obj_set_hidden(s_marker, true);
        show_summary(s_last);
    }
}

void show_radar_photo(const char *hex, const void *pixels, int width, int height)
{
    if (s_photo == nullptr || hex == nullptr || std::strcmp(hex, s_chosen) != 0) {
        return;  // the tap has moved on
    }
    if (pixels == nullptr || width <= 0 || height <= 0) {
        lv_obj_set_hidden(s_photo, true);
        return;
    }

    s_photo_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc.header.w      = static_cast<std::uint32_t>(width);
    s_photo_dsc.header.h      = static_cast<std::uint32_t>(height);
    s_photo_dsc.header.stride = static_cast<std::uint32_t>(width) * 2;
    s_photo_dsc.data_size     = static_cast<std::uint32_t>(width * height * 2);
    s_photo_dsc.data          = static_cast<const std::uint8_t *>(pixels);

    lv_image_set_src(s_photo, &s_photo_dsc);
    lv_obj_set_hidden(s_photo, false);
    lv_obj_invalidate(s_photo);
}

void show_radar_details(const char *hex, const radar::Details &details)
{
    if (hex == nullptr || std::strcmp(hex, s_chosen) != 0) {
        return;  // the tap has moved on
    }
    s_details = details;
    std::snprintf(s_details_hex, sizeof(s_details_hex), "%s", hex);
    show_radar(s_last);
}

}  // namespace ui
