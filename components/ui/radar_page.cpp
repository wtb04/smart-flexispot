#include "radar_page.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "map_data.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace ui {
namespace {
constexpr float DEG  = 3.14159265f / 180.0f;
// Must hold the gap plus a whole line box: at 24 the N and S labels landed
// outside the scope object and were clipped against the ring.
constexpr int MARK = 36;

constexpr int SPOKES = 8;
constexpr int RINGS  = 4;

constexpr int BOX = 24;  // local frame each outline is drawn in

constexpr int DRAWN_MAX = radar::kMaxAircraft;

constexpr int LABEL_MAX = 20;

// LVGL stores RGB565A8 as an RGB565 plane followed by a separate alpha plane,
// not interleaved.
constexpr std::size_t AIR_BYTES_PER_PX = 3;

constexpr int RIM_DOTS = 14;
constexpr int RIM_SIZE = 6;

struct Blip;

constexpr int READINGS = 5;

constexpr int MAP_POINTS     = 2400;
constexpr int MASK_MAX_EDGES = 64;

constexpr std::uint32_t INK_WATER = 0x5d93b8;

constexpr int RANGES[]     = {20, 40, 60, 80, 100, 120, 140, 160};
constexpr int RANGE_COUNT  = static_cast<int>(std::size(RANGES));
// The scope sits in a card the way the thermostat dial does, and the card's
// corners -- the room a circle leaves in a square -- carry the instrument's own
// controls and scale, set into the bezel like the distances are set on the rings.
constexpr std::int32_t BEZEL     = 20;
constexpr std::int32_t CORNER    = 88;
constexpr std::int32_t ZOOM_D    = theme::chip::size;
constexpr std::int32_t EDGE      = theme::chip::inset;  // as in every other card
constexpr float        KEY_FROM  = 120.0f;
constexpr float        KEY_TO    = 150.0f;
constexpr std::int32_t KEY_WIDTH = 5;

constexpr std::uint32_t ZOOM_MS = 260;

// Flat earth over eighty kilometres is off by less than the line width.
constexpr float KM_PER_LAT = 110.574f;
constexpr float KM_PER_LON = 111.320f;

constexpr int          SCALE_TOP_FT = 40000;
constexpr int          LEGEND_STEPS = 18;

constexpr float KM_PER_NM = 1.852f;

constexpr std::int32_t COLUMN_W   = 294;
constexpr std::int32_t COLUMN_GAP = theme::space::m;
constexpr std::int32_t ROW_H      = 30;
constexpr std::int32_t ROWS_Y     = 208;

// Both cards in the column are inset by the same amount, so the callsign, the
// readings and the photograph all start on one edge.
constexpr std::int32_t INSET   = 16;
constexpr std::int32_t INNER_W = COLUMN_W - 2 * INSET;

struct Blip {
    lv_obj_t *label;
};

constexpr int DOT_SIZE = 5;

enum class Shape : std::uint8_t { Airliner, Heavy, Light, Rotor, Other };

// Nose towards negative y: up, before the track rotation.
constexpr float OUTLINE_AIRLINER[][2] = {{0, -9}, {7, 4}, {0, 1}, {-7, 4}};
constexpr float OUTLINE_HEAVY[][2]    = {{0, -11}, {10, 5}, {0, 1}, {-10, 5}};
constexpr float OUTLINE_LIGHT[][2]    = {{0, -6}, {4, 5}, {-4, 5}};
constexpr float OUTLINE_OTHER[][2]    = {{0, -6}, {6, 0}, {0, 6}, {-6, 0}};
// One stroke through both diameters, because a line is a single polyline.
constexpr float OUTLINE_ROTOR[][2] = {{-7, -7}, {7, 7}, {0, 0}, {7, -7}, {-7, 7}};

struct Outline {
    const float (*points)[2];
    int  count;
    bool closed;
};

Outline outline_for(const char *category)
{
    Shape shape = Shape::Other;
    if (category[0] == 'A') {
        switch (category[1]) {
            case '1': shape = Shape::Light; break;
            case '4':
            case '5': shape = Shape::Heavy; break;
            case '7': shape = Shape::Rotor; break;
            default: shape = Shape::Airliner; break;
        }
    }
    switch (shape) {
        case Shape::Airliner:
            return {OUTLINE_AIRLINER, static_cast<int>(std::size(OUTLINE_AIRLINER)), true};
        case Shape::Heavy:
            return {OUTLINE_HEAVY, static_cast<int>(std::size(OUTLINE_HEAVY)), true};
        case Shape::Light:
            return {OUTLINE_LIGHT, static_cast<int>(std::size(OUTLINE_LIGHT)), true};
        case Shape::Rotor:
            return {OUTLINE_ROTOR, static_cast<int>(std::size(OUTLINE_ROTOR)), false};
        default:
            return {OUTLINE_OTHER, static_cast<int>(std::size(OUTLINE_OTHER)), true};
    }
}

struct Row {
    lv_obj_t *name;
    lv_obj_t *value;
};

lv_obj_t *s_scope  = nullptr;
lv_obj_t *s_marker = nullptr;
lv_obj_t   *s_rings[RINGS]  = {};
float       s_shown_range   = 0.0f;
int         s_zoom_from     = 0;
lv_obj_t   *s_zoom_in       = nullptr;
lv_obj_t   *s_zoom_out      = nullptr;
int         s_range_step    = 3;  // 80 km, which is where it starts

Blip              *s_blips = nullptr;
lv_obj_t          *s_rim[RIM_DOTS]              = {};
lv_obj_t          *s_legend[LEGEND_STEPS]       = {};

constexpr std::uint8_t WATER_ALPHA = 0x5c;

lv_obj_t     *s_air_canvas   = nullptr;
std::uint8_t *s_air_mask     = nullptr;
lv_obj_t     *s_water_canvas = nullptr;
lv_obj_t     *s_land_canvas  = nullptr;
std::uint8_t *s_water_mask   = nullptr;
std::uint8_t *s_land_mask    = nullptr;
std::int32_t  s_ground_side  = 0;

lv_point_precise_t *s_map_points = nullptr;
float               s_map_lat    = 0.0f;
float               s_map_lon    = 0.0f;
lv_point_precise_t s_spokes[SPOKES][2]          = {};

lv_obj_t *s_title      = nullptr;
lv_obj_t *s_operator   = nullptr;
lv_obj_t *s_airframe   = nullptr;
lv_obj_t *s_route      = nullptr;
lv_obj_t *s_cities     = nullptr;
lv_obj_t *s_summary    = nullptr;
lv_obj_t *s_photo       = nullptr;
lv_obj_t *s_photo_frame = nullptr;
lv_obj_t *s_photo_note  = nullptr;

std::int32_t s_photo_box_y = 0;
std::int32_t s_photo_box_h = 0;
lv_obj_t *s_nearby     = nullptr;

lv_image_dsc_t s_photo_dsc     = {};
Row            s_rows[READINGS] = {};

radar::Details s_details                     = {};
char           s_details_hex[radar::kHexLen] = {};

radar::Snapshot *s_last = nullptr;
char            s_chosen[radar::kHexLen] = {};

bool s_following = true;

enum class Picture : std::uint8_t { Idle, Looking, Loading, Missing, Shown };
Picture s_picture = Picture::Idle;

std::int32_t s_centre = 0;
std::int32_t s_radius = 0;

struct Plot {
    const radar::Aircraft *aircraft;
    float                  east_km;
    float                  north_km;
    std::int32_t           x;
    std::int32_t           y;
    std::int32_t           label_x;
    std::int32_t           label_w;
};

Plot *s_plots = nullptr;
int  s_shown  = 0;
int  s_beyond = 0;

constexpr int ALTITUDE_STOPS[] = {0, 12000, 25000, 40000};

constexpr std::uint32_t INK_UNKNOWN = 0x6b5a50;

std::uint32_t mix(std::uint32_t from, std::uint32_t to, float fraction)
{
    std::uint32_t blended = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        const auto  a     = static_cast<float>((from >> shift) & 0xff);
        const auto  b     = static_cast<float>((to >> shift) & 0xff);
        const float value = a + (b - a) * fraction;
        blended |= static_cast<std::uint32_t>(std::lround(value)) << shift;
    }
    return blended;
}

std::uint32_t altitude_ink(int feet)
{
    if (feet < 0) {
        return INK_UNKNOWN;
    }
    const std::uint32_t ink[] = {
        theme::dim_of(theme::primary_dim),          // low traffic sits back
        theme::primary,
        mix(theme::primary, theme::text, 0.45f),
        theme::text,                                // near white at cruise
    };
    const int count = static_cast<int>(std::size(ALTITUDE_STOPS));
    for (int i = 1; i < count; ++i) {
        if (feet <= ALTITUDE_STOPS[i]) {
            const float span = static_cast<float>(ALTITUDE_STOPS[i] - ALTITUDE_STOPS[i - 1]);
            const float frac = static_cast<float>(feet - ALTITUDE_STOPS[i - 1]) / span;
            return mix(ink[i - 1], ink[i], frac);
        }
    }
    return ink[count - 1];
}

const char *emergency(int squawk)
{
    switch (squawk) {
        case 7500: return "HIJACK";
        case 7600: return "NO RADIO";
        case 7700: return "EMERGENCY";
        default: return nullptr;
    }
}

std::int32_t text_width(const char *text, const lv_font_t *font)
{
    std::int32_t width = 0;
    for (const char *c = text; *c != '\0'; ++c) {
        width += lv_font_get_glyph_width(font, static_cast<std::uint32_t>(*c), 0);
    }
    return width;
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
    lv_obj_set_style_border_color(circle, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_border_opa(circle, opa, 0);
    lv_obj_set_style_border_width(circle, 2, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    quiet(circle);
}

constexpr std::int32_t COMPASS_GAP = 10;

void compass(lv_obj_t *parent, const char *text, char side)
{
    lv_obj_t          *label = theme::make_label(parent, text, theme::secondary,
                                                  fonts::size_16());
    const std::int32_t w     = text_width(text, fonts::size_16());
    const std::int32_t h     = fonts::size_16()->line_height;
    const std::int32_t edge  = s_radius + COMPASS_GAP;

    lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
    switch (side) {
        case 'N': lv_obj_set_pos(label, s_centre - w / 2, s_centre - edge - h); break;
        case 'S': lv_obj_set_pos(label, s_centre - w / 2, s_centre + edge); break;
        case 'E': lv_obj_set_pos(label, s_centre + edge, s_centre - h / 2); break;
        default: lv_obj_set_pos(label, s_centre - edge - w, s_centre - h / 2); break;
    }
    quiet(label);
}

constexpr float LABEL_ANGLE = 45.0f * DEG;

void place_ring_label(lv_obj_t *label, std::int32_t radius)
{
    const std::int32_t w = text_width(lv_label_get_text(label), fonts::size_16()) + 8;
    const std::int32_t h = fonts::size_16()->line_height;
    const auto         x = static_cast<std::int32_t>(s_centre + std::sin(LABEL_ANGLE) * radius);
    const auto         y = static_cast<std::int32_t>(s_centre - std::cos(LABEL_ANGLE) * radius);
    lv_obj_set_pos(label, x - w / 2, y - h / 2);
}

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
        lv_obj_set_style_line_color(spoke, lv_color_hex(theme::secondary), 0);
        lv_obj_set_style_line_opa(spoke, LV_OPA_10, 0);
        quiet(spoke);
    }

    constexpr lv_opa_t WEIGHT[RINGS] = {LV_OPA_10, LV_OPA_20, LV_OPA_30, LV_OPA_40};
    for (int i = 1; i <= RINGS; ++i) {
        const std::int32_t radius = s_radius * i / RINGS;
        ring(scope, radius, WEIGHT[i - 1]);

        char text[12];
        std::snprintf(text, sizeof(text), "%d", range_km * i / RINGS);
        lv_obj_t *label = theme::make_label(scope, text, theme::secondary, fonts::size_16());
        lv_obj_set_style_text_opa(label, LV_OPA_40, 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(theme::background), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(label, 4, 0);
        place_ring_label(label, radius);
        quiet(label);
        s_rings[i - 1] = label;
    }
}

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

    s_following = false;
    if (best < 0) {
        s_chosen[0]      = '\0';
        s_details        = radar::Details{};
        s_details_hex[0] = '\0';
        s_picture        = Picture::Idle;
    } else {
        std::memcpy(s_chosen, s_plots[best].aircraft->hex, sizeof(s_chosen));
        s_chosen[sizeof(s_chosen) - 1] = '\0';
        if (std::strcmp(s_details_hex, s_chosen) != 0) {
            s_details        = radar::Details{};
            s_details_hex[0] = '\0';
            s_picture        = Picture::Looking;
            radar::request_details(s_chosen, s_plots[best].aircraft->flight);
        }
    }
    show_radar(*s_last);
}

// The altitude scale runs along the rim band, the ring outside the last range
// ring where N, S, E and W already sit -- part of the scope rather than a key
// beside it. 0 at the steep end, 40k along the flatter bottom where a longer
// label lies easily, and the labels continue the arc instead of standing apart.
void build_key(lv_obj_t *scope)
{
    // Centred in the rim band on the same circle as the compass letters, so the
    // scale and N, S, E, W read as one set of markings.
    const std::int32_t mid   = s_radius + COMPASS_GAP + fonts::size_16()->line_height / 2;
    const std::int32_t outer = mid + KEY_WIDTH / 2;
    const float        step  = (KEY_TO - KEY_FROM) / LEGEND_STEPS;

    for (int i = 0; i < LEGEND_STEPS; ++i) {
        lv_obj_t *seg = lv_arc_create(scope);
        lv_obj_set_size(seg, 2 * outer, 2 * outer);
        lv_obj_set_pos(seg, s_centre - outer, s_centre - outer);
        lv_arc_set_bg_angles(seg, static_cast<lv_value_precise_t>(KEY_TO - (i + 1) * step - 0.6f),
                             static_cast<lv_value_precise_t>(KEY_TO - i * step));
        lv_obj_set_style_arc_width(seg, KEY_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(seg, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(seg, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(seg, 0, LV_PART_KNOB);
        quiet(seg);
        s_legend[i] = seg;
    }

    // Set just past each end of the arc, on the same circle, turned to follow it.
    auto end_label = [&](const char *text, float beyond, float sign) {
        const std::int32_t w     = text_width(text, fonts::size_16());
        const std::int32_t h     = fonts::size_16()->line_height;
        const float        half  = static_cast<float>(w) / 2.0f / static_cast<float>(mid) / DEG;
        const float        angle = beyond + sign * (half + 2.0f);
        const float        rad   = angle * DEG;
        lv_obj_t *label = theme::make_label(scope, text, theme::secondary, fonts::size_16());
        lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
        lv_obj_set_pos(label, s_centre + static_cast<std::int32_t>(std::cos(rad) * mid) - w / 2,
                       s_centre + static_cast<std::int32_t>(std::sin(rad) * mid) - h / 2);
        lv_obj_set_style_transform_pivot_x(label, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(label, h / 2, 0);
        lv_obj_set_style_transform_rotation(
            label, static_cast<std::int32_t>((angle - 90.0f) * 10.0f), 0);
        quiet(label);
    };
    end_label("0", KEY_TO, 1.0f);
    end_label("40k ft", KEY_FROM, -1.0f);
}

void mask_line(std::uint8_t *mask, std::int32_t x0, std::int32_t y0, std::int32_t x1,
               std::int32_t y1, std::uint8_t value)
{
    const std::int32_t dx    = std::abs(x1 - x0);
    const std::int32_t dy    = -std::abs(y1 - y0);
    const std::int32_t step_x = x0 < x1 ? 1 : -1;
    const std::int32_t step_y = y0 < y1 ? 1 : -1;
    std::int32_t       error  = dx + dy;
    const float        limit  = static_cast<float>(s_radius) * static_cast<float>(s_radius);

    for (;;) {
        const float ox = static_cast<float>(x0 - s_centre);
        const float oy = static_cast<float>(y0 - s_centre);
        if (ox * ox + oy * oy <= limit && x0 >= 0 && x0 < s_ground_side && y0 >= 0 &&
            y0 < s_ground_side) {
            mask[static_cast<std::size_t>(y0) * s_ground_side + x0] = value;
        }
        if (x0 == x1 && y0 == y1) {
            return;
        }
        const std::int32_t twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += step_x;
        }
        if (twice <= dx) {
            error += dx;
            y0 += step_y;
        }
    }
}

void fill_water(const lv_point_precise_t *points, const std::uint16_t *rings, int ring_count,
                int count)
{
    if (s_water_mask == nullptr || count < 3) {
        return;
    }
    const float limit = static_cast<float>(s_radius) * static_cast<float>(s_radius);

    std::int32_t top    = points[0].y;
    std::int32_t bottom = points[0].y;
    for (int i = 1; i < count; ++i) {
        top    = std::min<std::int32_t>(top, points[i].y);
        bottom = std::max<std::int32_t>(bottom, points[i].y);
    }
    top    = std::max<std::int32_t>(top, s_centre - s_radius);
    bottom = std::min<std::int32_t>(bottom, s_centre + s_radius);

    for (std::int32_t y = top; y <= bottom; ++y) {
        std::int32_t crossings[MASK_MAX_EDGES];
        int          found = 0;
        int          base  = 0;
        for (int r = 0; r < ring_count; ++r) {
            const int length = rings[r];
            for (int i = 0, j = length - 1; i < length && found < MASK_MAX_EDGES; j = i++) {
                const auto yi = static_cast<float>(points[base + i].y);
                const auto yj = static_cast<float>(points[base + j].y);
                if ((yi > static_cast<float>(y)) == (yj > static_cast<float>(y))) {
                    continue;
                }
                const auto  xi = static_cast<float>(points[base + i].x);
                const auto  xj = static_cast<float>(points[base + j].x);
                const float t  = (static_cast<float>(y) - yi) / (yj - yi);
                crossings[found++] = static_cast<std::int32_t>(std::lround(xi + t * (xj - xi)));
            }
            base += length;
        }
        std::sort(crossings, crossings + found);

        const float dy   = static_cast<float>(y - s_centre);
        const float span = limit - dy * dy;
        if (span < 0.0f) {
            continue;
        }
        const auto         half = static_cast<std::int32_t>(std::sqrt(span));
        const std::int32_t low  = std::max<std::int32_t>(s_centre - half, 0);
        const std::int32_t high = std::min<std::int32_t>(s_centre + half, s_ground_side - 1);

        for (int i = 0; i + 1 < found; i += 2) {
            const std::int32_t from = std::max(crossings[i], low);
            const std::int32_t to   = std::min(crossings[i + 1], high);
            if (to < from) {
                continue;
            }
            std::memset(s_water_mask + static_cast<std::size_t>(y) * s_ground_side + from,
                        WATER_ALPHA, static_cast<std::size_t>(to - from + 1));
        }
    }
}

void draw_map(float home_lat, float home_lon, int range_km)
{
    if (s_water_mask == nullptr || s_land_mask == nullptr || s_map_points == nullptr) {
        return;
    }

    const auto  area = static_cast<std::size_t>(s_ground_side) * s_ground_side;
    std::memset(s_water_mask, 0, area);
    std::memset(s_land_mask, 0, area);

    const float scale   = static_cast<float>(s_radius) / static_cast<float>(range_km);
    const float per_lat = KM_PER_LAT * scale;
    const float per_lon = KM_PER_LON * std::cos(home_lat * DEG) * scale;
    const auto  centre  = static_cast<float>(s_centre);
    const auto  reach   = static_cast<float>(s_radius);

    for (int i = 0; i < radar::kMapPathCount; ++i) {
        const radar::MapPath &path = radar::kMapPaths[i];
        const bool            fill = path.layer == radar::MapLayer::Ocean ||
                                     path.layer == radar::MapLayer::Lake;

        int count = 0;
        for (int v = 0; v < path.count && count < MAP_POINTS; ++v) {
            const float lat = static_cast<float>(path.points[v * 2]) / 1000000.0f;
            const float lon = static_cast<float>(path.points[v * 2 + 1]) / 1000000.0f;
            const float x   = centre + (lon - home_lon) * per_lon;
            const float y   = centre - (lat - home_lat) * per_lat;

            const float clamped_x = std::clamp(x, centre - reach * 4.0f, centre + reach * 4.0f);
            const float clamped_y = std::clamp(y, centre - reach * 4.0f, centre + reach * 4.0f);
            s_map_points[count++] = {static_cast<std::int32_t>(std::lround(clamped_x)),
                                     static_cast<std::int32_t>(std::lround(clamped_y))};
        }

        if (fill) {
            fill_water(s_map_points, path.rings, path.ring_count, count);
            continue;
        }

        std::uint8_t *mask  = path.layer == radar::MapLayer::River ? s_water_mask : s_land_mask;
        const auto    value = static_cast<std::uint8_t>(
            path.layer == radar::MapLayer::Province ? 0x38 : 0x70);
        for (int v = 1; v < count; ++v) {
            mask_line(mask, s_map_points[v - 1].x, s_map_points[v - 1].y, s_map_points[v].x,
                      s_map_points[v].y, value);
        }
    }

    if (s_water_canvas != nullptr) {
        lv_obj_invalidate(s_water_canvas);
    }
    if (s_land_canvas != nullptr) {
        lv_obj_invalidate(s_land_canvas);
    }
}

void set_button_enabled(lv_obj_t *button, bool enabled)
{
    if (button == nullptr) {
        return;
    }
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
        return;
    }
    lv_obj_remove_state(button, LV_STATE_PRESSED);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_remove_state(lv_obj_get_child(button, i), LV_STATE_PRESSED);
    }
    lv_obj_add_state(button, LV_STATE_DISABLED);
}

void paint_range_buttons()
{
    set_button_enabled(s_zoom_in, s_range_step > 0);
    set_button_enabled(s_zoom_out, s_range_step < RANGE_COUNT - 1);
}

void scale_ground(float factor)
{
    const auto zoom = static_cast<std::uint32_t>(std::lround(factor * 256.0f));
    for (lv_obj_t *canvas : {s_water_canvas, s_land_canvas}) {
        if (canvas != nullptr) {
            lv_image_set_pivot(canvas, s_centre, s_centre);
            lv_image_set_scale(canvas, zoom);
        }
    }
}

void place_blip(Plot &plot, float range_km);

void move_blips(float range_km, float factor)
{
    (void)range_km;
    if (s_air_canvas != nullptr) {
        lv_image_set_pivot(s_air_canvas, s_centre, s_centre);
        lv_image_set_scale(s_air_canvas, static_cast<std::uint32_t>(std::lround(factor * 256.0f)));
    }
    for (int i = 0; i < LABEL_MAX; ++i) {
        lv_obj_set_hidden(s_blips[i].label, true);
    }
    lv_obj_set_hidden(s_marker, true);
}

void zoom_step(void *, std::int32_t value)
{
    const auto  from = static_cast<float>(s_zoom_from);
    const auto  to   = static_cast<float>(RANGES[s_range_step]);
    const float t    = static_cast<float>(value) / 256.0f;

    s_shown_range = from + (to - from) * t;
    scale_ground(from / s_shown_range);
    move_blips(s_shown_range, from / s_shown_range);
}

void zoom_done(lv_anim_t *)
{
    const int settled = RANGES[s_range_step];
    s_shown_range     = static_cast<float>(settled);
    scale_ground(1.0f);
    if (s_map_lat != 0.0f || s_map_lon != 0.0f) {
        draw_map(s_map_lat, s_map_lon, settled);
    }
    show_radar(*s_last);
}

void apply_range(int from_km)
{
    const int settled = RANGES[s_range_step];
    for (int i = 1; i <= RINGS; ++i) {
        char text[12];
        std::snprintf(text, sizeof(text), "%d", settled * i / RINGS);
        theme::set_text(s_rings[i - 1], text);
        place_ring_label(s_rings[i - 1], s_radius * i / RINGS);
    }
    paint_range_buttons();

    if (from_km <= 0 || s_scope == nullptr) {
        zoom_done(nullptr);
        return;
    }

    s_zoom_from = from_km;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_scope);
    lv_anim_set_values(&anim, 0, 256);
    lv_anim_set_duration(&anim, ZOOM_MS);
    lv_anim_set_exec_cb(&anim, zoom_step);
    lv_anim_set_completed_cb(&anim, zoom_done);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

void range_clicked(lv_event_t *event)
{
    const auto step =
        static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event)));
    const int next = std::clamp(s_range_step + step, 0, RANGE_COUNT - 1);
    if (next == s_range_step) {
        return;
    }
    const int from = s_shown_range > 0.0f ? static_cast<int>(std::lround(s_shown_range))
                                          : RANGES[s_range_step];
    s_range_step   = next;
    apply_range(from);
}

// Round chips in the top corners, the same as the heating card's: a dot of the
// darker surface on the lighter card is what makes it read as something to
// press. The hit area reaches past the dot, so a finger does not have to find it.
lv_obj_t *zoom_chip(lv_obj_t *bezel, std::int32_t x, const char *text, int step)
{
    lv_obj_t *chip = theme::make_chip(bezel, text);
    lv_obj_set_pos(chip, x, EDGE);
    lv_obj_set_ext_click_area(chip, (CORNER - ZOOM_D) / 2);
    lv_obj_add_event_cb(chip, range_clicked, LV_EVENT_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(step)));
    return chip;
}

void build_zoom(lv_obj_t *bezel, std::int32_t side)
{
    s_zoom_out = zoom_chip(bezel, EDGE, LV_SYMBOL_MINUS, 1);
    s_zoom_in  = zoom_chip(bezel, side - ZOOM_D - EDGE, LV_SYMBOL_PLUS, -1);
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

    lv_obj_t *ground = lv_obj_create(scope);
    lv_obj_set_pos(ground, 0, 0);
    lv_obj_set_size(ground, side, side);
    lv_obj_set_style_bg_opa(ground, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ground, 0, 0);
    lv_obj_set_style_pad_all(ground, 0, 0);
    quiet(ground);

    s_ground_side = side;
    const auto area = static_cast<std::size_t>(side) * side;

    s_water_mask = static_cast<std::uint8_t *>(heap_caps_calloc(area, 1, MALLOC_CAP_SPIRAM));
    s_land_mask  = static_cast<std::uint8_t *>(heap_caps_calloc(area, 1, MALLOC_CAP_SPIRAM));
    s_map_points = static_cast<lv_point_precise_t *>(heap_caps_malloc(
        sizeof(lv_point_precise_t) * MAP_POINTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (s_water_mask != nullptr && s_land_mask != nullptr) {
        s_water_canvas = lv_canvas_create(ground);
        lv_canvas_set_buffer(s_water_canvas, s_water_mask, side, side, LV_COLOR_FORMAT_A8);
        lv_obj_set_style_image_recolor(s_water_canvas, lv_color_hex(INK_WATER), 0);
        lv_obj_set_style_image_recolor_opa(s_water_canvas, LV_OPA_COVER, 0);
        quiet(s_water_canvas);

        s_land_canvas = lv_canvas_create(ground);
        lv_canvas_set_buffer(s_land_canvas, s_land_mask, side, side, LV_COLOR_FORMAT_A8);
        lv_obj_set_style_image_recolor(s_land_canvas, lv_color_hex(theme::secondary), 0);
        lv_obj_set_style_image_recolor_opa(s_land_canvas, LV_OPA_COVER, 0);
        quiet(s_land_canvas);
    }

    build_chart(scope, range_km);

    compass(scope, "N", 'N');
    compass(scope, "S", 'S');
    compass(scope, "E", 'E');
    compass(scope, "W", 'W');

    lv_obj_t *home = lv_obj_create(scope);
    lv_obj_set_size(home, 8, 8);
    lv_obj_set_pos(home, s_centre - 4, s_centre - 4);
    theme::style_panel(home, theme::panel, 4);
    theme::fill_accent(home);
    quiet(home);

    s_marker = lv_obj_create(scope);
    lv_obj_set_size(s_marker, 34, 34);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_marker, lv_color_hex(theme::primary), 0);
    lv_obj_set_style_border_width(s_marker, 2, 0);
    lv_obj_set_style_pad_all(s_marker, 0, 0);
    lv_obj_set_hidden(s_marker, true);
    quiet(s_marker);

    for (lv_obj_t *&dot : s_rim) {
        dot = lv_obj_create(scope);
        lv_obj_set_size(dot, RIM_SIZE, RIM_SIZE);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_50, 0);
        lv_obj_set_hidden(dot, true);
        quiet(dot);
    }

    s_air_mask = static_cast<std::uint8_t *>(heap_caps_calloc(
        static_cast<std::size_t>(side) * side, AIR_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    if (s_air_mask != nullptr) {
        s_air_canvas = lv_canvas_create(scope);
        lv_canvas_set_buffer(s_air_canvas, s_air_mask, side, side, LV_COLOR_FORMAT_RGB565A8);
        lv_obj_set_pos(s_air_canvas, 0, 0);
        quiet(s_air_canvas);
    }

    for (int i = 0; i < LABEL_MAX; ++i) {
        s_blips[i].label = theme::make_label(scope, "", theme::secondary, fonts::size_16());
        lv_obj_set_hidden(s_blips[i].label, true);
        quiet(s_blips[i].label);
    }

    s_scope = scope;
}

lv_obj_t *stacked(lv_obj_t *parent, std::int32_t y, std::uint32_t colour, const lv_font_t *font)
{
    lv_obj_t *label = theme::make_label(parent, "", colour, font);
    lv_obj_set_pos(label, 0, y);
    lv_obj_set_width(label, COLUMN_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, font->line_height);
    quiet(label);
    return label;
}

void build_column(lv_obj_t *parent, std::int32_t x, std::int32_t height)
{
    lv_obj_t *column = lv_obj_create(parent);
    lv_obj_set_pos(column, x, 0);
    lv_obj_set_size(column, COLUMN_W, height);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(column, 0, 0);
    lv_obj_set_style_pad_all(column, 0, 0);
    quiet(column);

    // One aircraft is one card: who it is, where it is going, the readings and the
    // photograph. The same surface as the scope beside it -- neither half of the
    // page is louder than the other.
    lv_obj_t *identity = theme::make_card(column);
    lv_obj_set_pos(identity, 0, 0);
    lv_obj_set_size(identity, COLUMN_W, height);
    lv_obj_set_style_pad_all(identity, INSET, 0);
    quiet(identity);

    auto put = [&](std::int32_t y, std::uint32_t colour, const lv_font_t *font) {
        lv_obj_t *label = theme::make_label(identity, "", colour, font);
        lv_obj_set_pos(label, 0, y);
        lv_obj_set_width(label, INNER_W);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(label, font->line_height);
        quiet(label);
        return label;
    };
    s_title = put(0, theme::text, fonts::size_32());
    theme::ink_accent(s_title);
    s_operator = put(42, theme::text, fonts::size_20());
    s_airframe = put(68, theme::secondary, fonts::size_16());
    s_route    = put(98, theme::text, fonts::size_28());
    s_cities   = put(134, theme::secondary, fonts::size_16());

    const std::int32_t card_bottom = height;

    for (int i = 0; i < READINGS; ++i) {
        const std::int32_t y = ROWS_Y + INSET + i * ROW_H;

        s_rows[i].name = theme::make_label(column, "", theme::secondary, fonts::size_16());
        lv_obj_set_pos(s_rows[i].name, INSET, y + 4);
        quiet(s_rows[i].name);

        s_rows[i].value = theme::make_label(column, "--", theme::text, fonts::size_22());
        lv_obj_set_pos(s_rows[i].value, INSET, y);
        lv_obj_set_width(s_rows[i].value, INNER_W);
        lv_obj_set_style_text_align(s_rows[i].value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(s_rows[i].value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_rows[i].value, fonts::size_22()->line_height);
        quiet(s_rows[i].value);
    }

    const std::int32_t photo_y = ROWS_Y + INSET + READINGS * ROW_H + theme::space::s;
    const std::int32_t photo_h = card_bottom - INSET - photo_y;

    s_photo_box_y = photo_y;
    s_photo_box_h = photo_h;

    s_photo_frame = lv_obj_create(column);
    lv_obj_set_pos(s_photo_frame, INSET, photo_y);
    lv_obj_set_size(s_photo_frame, INNER_W, photo_h);
    lv_obj_set_style_bg_opa(s_photo_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_photo_frame, 0, 0);
    lv_obj_set_style_pad_all(s_photo_frame, 0, 0);
    lv_obj_set_style_radius(s_photo_frame, 12, 0);
    lv_obj_set_style_clip_corner(s_photo_frame, true, 0);
    lv_obj_set_hidden(s_photo_frame, true);
    quiet(s_photo_frame);

    s_photo = lv_image_create(s_photo_frame);
    lv_obj_set_pos(s_photo, 0, 0);
    lv_image_set_inner_align(s_photo, LV_IMAGE_ALIGN_CONTAIN);
    quiet(s_photo);

    s_nearby = theme::make_label(column, "", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_nearby, INSET, photo_y);
    lv_obj_set_width(s_nearby, INNER_W);
    lv_obj_set_style_text_line_space(s_nearby, 8, 0);
    lv_obj_set_hidden(s_nearby, true);
    quiet(s_nearby);

    s_photo_note = theme::make_label(column, "", theme::secondary, fonts::size_16());
    lv_obj_set_style_text_opa(s_photo_note, LV_OPA_60, 0);
    lv_obj_set_pos(s_photo_note, INSET, photo_y + photo_h / 2 - 10);
    lv_obj_set_width(s_photo_note, INNER_W);
    lv_obj_set_style_text_align(s_photo_note, LV_TEXT_ALIGN_CENTER, 0);
    quiet(s_photo_note);

}

void show_picture()
{
    const char *note = nullptr;
    switch (s_picture) {
        case Picture::Looking: note = "looking up"; break;
        case Picture::Loading: note = "loading photo"; break;
        case Picture::Missing: note = "no photo on file"; break;
        default: break;
    }
    theme::set_text(s_photo_note, note != nullptr ? note : "");
    lv_obj_set_hidden(s_photo_frame, s_picture != Picture::Shown);
}

const char *blip_name(const radar::Aircraft &aircraft)
{
    return aircraft.flight[0] != '\0' ? aircraft.flight : aircraft.hex;
}

void place_blip(Plot &plot, float range_km)
{
    const float scale = static_cast<float>(s_radius) / range_km;
    plot.x = static_cast<std::int32_t>(std::lround(s_centre + plot.east_km * scale));
    plot.y = static_cast<std::int32_t>(std::lround(s_centre - plot.north_km * scale));

    const std::int32_t span = text_width(blip_name(*plot.aircraft), fonts::size_16());
    plot.label_x = plot.x > s_centre ? plot.x - 11 - span : plot.x + 11;
    plot.label_w = span;
}

std::uint16_t to_rgb565(std::uint32_t colour)
{
    return static_cast<std::uint16_t>((((colour >> 16) & 0xff) >> 3) << 11 |
                                      (((colour >> 8) & 0xff) >> 2) << 5 |
                                      ((colour & 0xff) >> 3));
}

void air_pixels(std::int32_t y, std::int32_t from, std::int32_t to, std::uint16_t ink)
{
    auto *colour = reinterpret_cast<std::uint16_t *>(s_air_mask) +
                   static_cast<std::size_t>(y) * s_ground_side;
    std::uint8_t *alpha = s_air_mask + static_cast<std::size_t>(s_ground_side) * s_ground_side * 2 +
                          static_cast<std::size_t>(y) * s_ground_side;
    for (std::int32_t x = from; x <= to; ++x) {
        colour[x] = ink;
    }
    std::memset(alpha + from, 0xff, static_cast<std::size_t>(to - from + 1));
}

void fill_blip(const lv_point_precise_t *points, int count, std::uint16_t ink)
{
    if (s_air_mask == nullptr || count < 3) {
        return;
    }
    std::int32_t top    = points[0].y;
    std::int32_t bottom = points[0].y;
    for (int i = 1; i < count; ++i) {
        top    = std::min<std::int32_t>(top, points[i].y);
        bottom = std::max<std::int32_t>(bottom, points[i].y);
    }
    top    = std::max<std::int32_t>(top, 0);
    bottom = std::min<std::int32_t>(bottom, s_ground_side - 1);

    constexpr int MAX_CROSSINGS = 12;

    for (std::int32_t y = top; y <= bottom; ++y) {
        std::int32_t crossings[MAX_CROSSINGS];
        int          found = 0;
        for (int i = 0, j = count - 1; i < count && found < MAX_CROSSINGS; j = i++) {
            const auto yi = static_cast<float>(points[i].y);
            const auto yj = static_cast<float>(points[j].y);
            if ((yi > static_cast<float>(y)) == (yj > static_cast<float>(y))) {
                continue;
            }
            const auto  xi = static_cast<float>(points[i].x);
            const auto  xj = static_cast<float>(points[j].x);
            const float t  = (static_cast<float>(y) - yi) / (yj - yi);
            crossings[found++] = static_cast<std::int32_t>(std::lround(xi + t * (xj - xi)));
        }
        for (int i = 1; i < found; ++i) {
            const std::int32_t value = crossings[i];
            int                j     = i - 1;
            while (j >= 0 && crossings[j] > value) {
                crossings[j + 1] = crossings[j];
                --j;
            }
            crossings[j + 1] = value;
        }
        for (int i = 0; i + 1 < found; i += 2) {
            const std::int32_t from = std::max<std::int32_t>(crossings[i], 0);
            const std::int32_t to   = std::min<std::int32_t>(crossings[i + 1], s_ground_side - 1);
            if (to < from) {
                continue;
            }
            air_pixels(y, from, to, ink);
        }
    }
}

void raster_blip(const Plot &plot)
{
    const float track = plot.aircraft->track_deg < 0.0f ? 0.0f : plot.aircraft->track_deg;
    const float sin_t = std::sin(track * DEG);
    const float cos_t = std::cos(track * DEG);

    const Outline      outline = outline_for(plot.aircraft->category);
    lv_point_precise_t shape[8];
    for (int i = 0; i < outline.count; ++i) {
        const float px = outline.points[i][0] * cos_t - outline.points[i][1] * sin_t;
        const float py = outline.points[i][0] * sin_t + outline.points[i][1] * cos_t;
        shape[i] = {plot.x + static_cast<std::int32_t>(std::lround(px)),
                    plot.y + static_cast<std::int32_t>(std::lround(py))};
    }

    const bool          shout = emergency(plot.aircraft->squawk) != nullptr;
    const std::uint16_t ink =
        to_rgb565(shout ? theme::red : altitude_ink(plot.aircraft->altitude_ft));

    if (outline.closed) {
        fill_blip(shape, outline.count, ink);
    }

    for (std::int32_t dy = -DOT_SIZE / 2; dy <= DOT_SIZE / 2; ++dy) {
        const std::int32_t y = plot.y + dy;
        if (y < 0 || y >= s_ground_side) {
            continue;
        }
        const std::int32_t from = std::max<std::int32_t>(plot.x - DOT_SIZE / 2, 0);
        const std::int32_t to   = std::min<std::int32_t>(plot.x + DOT_SIZE / 2, s_ground_side - 1);
        if (to >= from) {
            air_pixels(y, from, to, ink);
        }
    }
}

void draw_label(Blip &blip, const Plot &plot)
{
    const bool shout = emergency(plot.aircraft->squawk) != nullptr;
    theme::set_text(blip.label, blip_name(*plot.aircraft));
    theme::set_text_color(blip.label, shout ? theme::red : theme::secondary);
    lv_obj_set_pos(blip.label, plot.label_x, plot.y - 8);
    lv_obj_set_hidden(blip.label, false);
}

void draw_rim(lv_obj_t *dot, const radar::Aircraft &aircraft)
{
    const float radians = aircraft.bearing_deg * DEG;
    const auto  reach   = static_cast<float>(s_radius);
    const auto  x = static_cast<std::int32_t>(std::lround(s_centre + std::sin(radians) * reach));
    const auto  y = static_cast<std::int32_t>(std::lround(s_centre - std::cos(radians) * reach));

    lv_obj_set_pos(dot, x - RIM_SIZE / 2, y - RIM_SIZE / 2);
    theme::set_bg_color(dot, altitude_ink(aircraft.altitude_ft));
    lv_obj_set_hidden(dot, false);
}

void set_row(int index, const char *name, const char *value)
{
    theme::set_text(s_rows[index].name, name);
    theme::set_text(s_rows[index].value, value);
    theme::set_text_color(s_rows[index].value, theme::text);
}

void show_summary(const radar::Snapshot &snapshot)
{
    char text[128];

    std::snprintf(text, sizeof(text), "%d aircraft", s_shown);
    theme::set_text(s_title, s_shown > 0 ? text : "Quiet sky");
    theme::set_text(s_operator, s_shown > 0 ? "overhead now" : "nothing within range");

    theme::set_text(s_airframe, "");
    theme::set_text(s_route, "");
    theme::set_text(s_cities, s_shown > 0 ? "Tap one for its details" : "");

    const radar::Aircraft *highest = nullptr;
    const radar::Aircraft *lowest  = nullptr;
    const radar::Aircraft *fastest = nullptr;
    for (int i = 0; i < s_shown; ++i) {
        const radar::Aircraft *a = s_plots[i].aircraft;
        if (a->altitude_ft >= 0 && (highest == nullptr || a->altitude_ft > highest->altitude_ft)) {
            highest = a;
        }
        if (a->altitude_ft >= 0 && (lowest == nullptr || a->altitude_ft < lowest->altitude_ft)) {
            lowest = a;
        }
        if (fastest == nullptr || a->speed_kt > fastest->speed_kt) {
            fastest = a;
        }
    }

    if (s_shown > 0) {
        set_row(0, "Nearest", blip_name(*s_plots[0].aircraft));
        std::snprintf(text, sizeof(text), "%.1f km",
                      static_cast<double>(s_plots[0].aircraft->distance_nm * KM_PER_NM));
        set_row(1, "Distance", text);
        if (highest != nullptr) {
            std::snprintf(text, sizeof(text), "%d ft", highest->altitude_ft);
        }
        set_row(2, "Highest", highest != nullptr ? text : "--");
        if (lowest != nullptr) {
            std::snprintf(text, sizeof(text), "%d ft", lowest->altitude_ft);
        }
        set_row(3, "Lowest", lowest != nullptr ? text : "--");
        std::snprintf(text, sizeof(text), "%.0f kt", static_cast<double>(fastest->speed_kt));
        set_row(4, "Fastest", text);
    } else {
        static const char *IDLE[READINGS] = {"Nearest", "Distance", "Highest", "Lowest",
                                             "Fastest"};
        for (int i = 0; i < READINGS; ++i) {
            set_row(i, IDLE[i], "--");
        }
    }

    text[0]    = '\0';
    int length = 0;
    for (int i = 0; i < s_shown && i < 4; ++i) {
        const radar::Aircraft *a = s_plots[i].aircraft;
        length += std::snprintf(text + length, sizeof(text) - length, "%s%-9s%.0f km",
                                i > 0 ? "\n" : "", blip_name(*a),
                                static_cast<double>(a->distance_nm * KM_PER_NM));
    }
    theme::set_text(s_nearby, text);
    lv_obj_set_hidden(s_nearby, s_shown == 0);

    s_picture = Picture::Idle;
    show_picture();

    theme::set_text(s_summary, snapshot.age_s < 0 ? "waiting for a fix"
                               : !snapshot.ok    ? "feed unreachable"
                                                 : "");
    theme::set_text_color(s_summary, theme::amber);
}

void show_selected(const radar::Aircraft &aircraft)
{
    char text[96];

    lv_obj_set_hidden(s_nearby, true);
    theme::set_text(s_title, aircraft.flight[0] != '\0' ? aircraft.flight : aircraft.hex);

    const bool mine = std::strcmp(s_details_hex, aircraft.hex) == 0;

    if (mine && s_details.airline[0] != '\0') {
        theme::set_text(s_operator, s_details.airline);
    } else if (mine && s_details.owner[0] != '\0') {
        theme::set_text(s_operator, s_details.owner);
    } else {
        theme::set_text(s_operator, "");
    }

    char shape[56] = {};
    if (mine && s_details.manufacturer[0] != '\0' && s_details.model[0] != '\0') {
        std::snprintf(shape, sizeof(shape), "%s %s", s_details.manufacturer, s_details.model);
    } else if (aircraft.desc[0] != '\0') {
        std::snprintf(shape, sizeof(shape), "%s", aircraft.desc);
    } else if (mine && s_details.model[0] != '\0') {
        std::snprintf(shape, sizeof(shape), "%s", s_details.model);
    } else {
        std::snprintf(shape, sizeof(shape), "%s", aircraft.type);
    }

    if (aircraft.reg[0] != '\0' && shape[0] != '\0') {
        std::snprintf(text, sizeof(text), "%s  ·  %s", aircraft.reg, shape);
    } else {
        std::snprintf(text, sizeof(text), "%s",
                      aircraft.reg[0] != '\0' ? aircraft.reg
                                              : (shape[0] != '\0' ? shape : aircraft.hex));
    }
    theme::set_text(s_airframe, text);

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

    static const char *NAMES[READINGS] = {"Distance", "Altitude", "Speed", "Track", "Squawk"};
    for (int i = 0; i < READINGS; ++i) {
        theme::set_text(s_rows[i].name, NAMES[i]);
    }

    std::snprintf(text, sizeof(text), "%.1f km",
                  static_cast<double>(aircraft.distance_nm * KM_PER_NM));
    theme::set_text(s_rows[0].value, text);

    if (aircraft.altitude_ft >= 0) {
        const char *trend = aircraft.vertical_fpm > 245    ? " " LV_SYMBOL_UP
                            : aircraft.vertical_fpm < -245 ? " " LV_SYMBOL_DOWN
                                                           : "";
        std::snprintf(text, sizeof(text), "%d ft%s", aircraft.altitude_ft, trend);
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    theme::set_text(s_rows[1].value, text);
    theme::set_text_color(s_rows[1].value, altitude_ink(aircraft.altitude_ft));

    std::snprintf(text, sizeof(text), "%.0f kt", static_cast<double>(aircraft.speed_kt));
    theme::set_text(s_rows[2].value, text);

    if (aircraft.track_deg >= 0.0f) {
        std::snprintf(text, sizeof(text), "%.0f°", static_cast<double>(aircraft.track_deg));
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    theme::set_text(s_rows[3].value, text);

    const char *shout = emergency(aircraft.squawk);
    if (aircraft.squawk >= 0) {
        std::snprintf(text, sizeof(text), "%04d", aircraft.squawk);
    } else {
        std::snprintf(text, sizeof(text), "--");
    }
    theme::set_text(s_rows[4].value, text);
    theme::set_text_color(s_rows[4].value, shout != nullptr ? theme::red : theme::text);

    show_picture();

    theme::set_text(s_summary, shout != nullptr ? shout : (s_last->ok ? "" : "feed unreachable"));
    theme::set_text_color(s_summary, shout != nullptr ? theme::red : theme::amber);
}

}  // namespace

void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    s_last = static_cast<radar::Snapshot *>(
        heap_caps_calloc(1, sizeof(radar::Snapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_blips = static_cast<Blip *>(
        heap_caps_calloc(LABEL_MAX, sizeof(Blip), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_plots = static_cast<Plot *>(
        heap_caps_calloc(DRAWN_MAX, sizeof(Plot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_last == nullptr || s_blips == nullptr || s_plots == nullptr) {
        return;
    }

    const std::int32_t card_w = width - COLUMN_W - COLUMN_GAP;
    const std::int32_t side   = std::min(card_w, height);
    const std::int32_t disc   = side - 2 * BEZEL;
    s_shown_range = static_cast<float>(RANGES[s_range_step]);
    lv_obj_t *bezel = theme::make_card(page);
    lv_obj_set_pos(bezel, 0, 0);
    lv_obj_set_size(bezel, card_w, height);
    lv_obj_set_style_pad_all(bezel, 0, 0);
    quiet(bezel);

    build_scope(bezel, disc, RANGES[s_range_step]);
    lv_obj_set_pos(s_scope, (card_w - disc) / 2, (height - disc) / 2);
    build_key(s_scope);
    build_zoom(bezel, card_w);
    lv_obj_set_pos(theme::make_screw(bezel, ZOOM_D), EDGE, height - ZOOM_D - EDGE);
    lv_obj_set_pos(theme::make_screw(bezel, ZOOM_D), card_w - ZOOM_D - EDGE,
                   height - ZOOM_D - EDGE);

    s_summary = theme::make_label(bezel, "", theme::amber, fonts::size_16());
    lv_obj_set_width(s_summary, CORNER + 60);
    lv_obj_set_style_text_align(s_summary, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_summary, fonts::size_16()->line_height);
    lv_obj_align(s_summary, LV_ALIGN_BOTTOM_RIGHT, -(EDGE + ZOOM_D + theme::space::s),
                 -(EDGE + (ZOOM_D - fonts::size_16()->line_height) / 2));
    quiet(s_summary);
    paint_range_buttons();
    build_column(page, card_w + COLUMN_GAP, height);
    show_radar(*s_last);
}

void radar_page_opened()
{
    s_following      = true;
    s_details_hex[0] = '\0';
}

void show_radar(const radar::Snapshot &snapshot)
{
    if (s_scope == nullptr || s_last == nullptr || s_blips == nullptr) {
        return;
    }
    if (&snapshot != s_last) {
        *s_last = snapshot;
    }

    const float range_km = s_shown_range > 0.0f ? s_shown_range
                                                : static_cast<float>(RANGES[s_range_step]);

    if (s_last->home_lat != s_map_lat || s_last->home_lon != s_map_lon) {
        s_map_lat = s_last->home_lat;
        s_map_lon = s_last->home_lon;
        if (s_map_lat != 0.0f || s_map_lon != 0.0f) {
            draw_map(s_map_lat, s_map_lon, range_km);
        }
    }

    s_shown      = 0;
    int rim_used = 0;
    for (int i = 0; i < s_last->count && s_shown < DRAWN_MAX; ++i) {
        const radar::Aircraft &aircraft = s_last->list[i];
        if (aircraft.on_ground) {
            continue;
        }
        const float distance_km = aircraft.distance_nm * KM_PER_NM;
        if (distance_km > range_km) {
            if (rim_used < RIM_DOTS) {
                draw_rim(s_rim[rim_used++], aircraft);
            }
            continue;
        }
        s_plots[s_shown].aircraft = &s_last->list[i];
        s_plots[s_shown].east_km  = distance_km * std::sin(aircraft.bearing_deg * DEG);
        s_plots[s_shown].north_km = distance_km * std::cos(aircraft.bearing_deg * DEG);
        ++s_shown;
    }
    for (int i = rim_used; i < RIM_DOTS; ++i) {
        lv_obj_set_hidden(s_rim[i], true);
    }
    s_beyond = rim_used;

    for (int i = 0; i < LEGEND_STEPS; ++i) {
        lv_obj_set_style_arc_color(
            s_legend[i], lv_color_hex(altitude_ink(SCALE_TOP_FT * i / (LEGEND_STEPS - 1))),
            LV_PART_MAIN);
    }

    std::sort(s_plots, s_plots + s_shown, [](const Plot &a, const Plot &b) {
        return a.aircraft->distance_nm < b.aircraft->distance_nm;
    });

    if (s_following) {
        if (s_shown > 0) {
            int held = -1;
            for (int i = 0; i < s_shown; ++i) {
                if (std::strcmp(s_chosen, s_plots[i].aircraft->hex) == 0) {
                    held = i;
                    break;
                }
            }
            const bool keep = held >= 0 && s_plots[held].aircraft->distance_nm <
                                               s_plots[0].aircraft->distance_nm * 1.2f;
            const int at = keep ? held : 0;
            if (!keep) {
                std::memcpy(s_chosen, s_plots[0].aircraft->hex, sizeof(s_chosen));
                s_chosen[sizeof(s_chosen) - 1] = '\0';
            }
            if (std::strcmp(s_details_hex, s_chosen) != 0) {
                s_picture = Picture::Looking;
                radar::request_details(s_chosen, s_plots[at].aircraft->flight);
            }
        } else {
            s_chosen[0] = '\0';
        }
    }

    for (int i = 0; i < s_shown; ++i) {
        place_blip(s_plots[i], range_km);
    }

    bool named[DRAWN_MAX];
    for (int i = 0; i < s_shown; ++i) {
        named[i] = true;
    }
    const std::int32_t line = fonts::size_16()->line_height;
    for (int i = 0; i < s_shown; ++i) {
        for (int j = i + 1; j < s_shown; ++j) {
            const bool apart =
                s_plots[i].label_x > s_plots[j].label_x + s_plots[j].label_w + 2 ||
                s_plots[j].label_x > s_plots[i].label_x + s_plots[i].label_w + 2 ||
                s_plots[i].y > s_plots[j].y + line || s_plots[j].y > s_plots[i].y + line;
            if (!apart) {
                named[i] = false;
                named[j] = false;
            }
        }
    }

    const lv_color_t accent = lv_color_hex(theme::primary);
    if (!theme::has_local_color(s_marker, LV_STYLE_BORDER_COLOR, 0, accent)) {
        lv_obj_set_style_border_color(s_marker, accent, 0);
    }

    if (s_air_mask != nullptr) {
        const auto pixels = static_cast<std::size_t>(s_ground_side) * s_ground_side;
        std::memset(s_air_mask + pixels * 2, 0, pixels);
    }
    for (int i = 0; i < LABEL_MAX; ++i) {
        lv_obj_set_hidden(s_blips[i].label, true);
    }

    const radar::Aircraft *chosen = nullptr;
    int                    labelled = 0;
    for (int i = 0; i < s_shown; ++i) {
        raster_blip(s_plots[i]);

        const bool mine =
            s_chosen[0] != '\0' && std::strcmp(s_chosen, s_plots[i].aircraft->hex) == 0;
        if (mine) {
            chosen = s_plots[i].aircraft;
            lv_obj_set_pos(s_marker, s_plots[i].x - 17, s_plots[i].y - 17);
            lv_obj_set_hidden(s_marker, false);
        }
        if ((named[i] || mine) && labelled < LABEL_MAX) {
            draw_label(s_blips[labelled++], s_plots[i]);
        }
    }
    if (s_air_canvas != nullptr) {
        lv_image_set_scale(s_air_canvas, 256);
        lv_obj_invalidate(s_air_canvas);
    }

    if (chosen != nullptr) {
        show_selected(*chosen);
    } else {
        s_chosen[0] = '\0';
        lv_obj_set_hidden(s_marker, true);
        show_summary(*s_last);
    }
}

void show_radar_photo(const char *hex, const void *pixels, int width, int height)
{

    if (s_photo == nullptr || hex == nullptr || std::strcmp(hex, s_chosen) != 0) {
        return;  // the tap has moved on
    }
    if (pixels == nullptr || width <= 0 || height <= 0) {
        s_picture = Picture::Missing;
        show_picture();
        return;
    }

    s_photo_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_photo_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_photo_dsc.header.w      = static_cast<std::uint32_t>(width);
    s_photo_dsc.header.h      = static_cast<std::uint32_t>(height);
    s_photo_dsc.header.stride = static_cast<std::uint32_t>(width) * 2;
    s_photo_dsc.data_size     = static_cast<std::uint32_t>(width * height * 2);
    s_photo_dsc.data          = static_cast<const std::uint8_t *>(pixels);

    const std::int32_t fit_w = std::min(INNER_W, width * s_photo_box_h / height);
    const std::int32_t fit_h = std::min(s_photo_box_h, height * INNER_W / width);
    lv_obj_set_size(s_photo_frame, fit_w, fit_h);
    lv_obj_set_pos(s_photo_frame, INSET + (INNER_W - fit_w) / 2,
                   s_photo_box_y + (s_photo_box_h - fit_h) / 2);
    lv_obj_set_size(s_photo, fit_w, fit_h);

    lv_image_set_src(s_photo, &s_photo_dsc);
    s_picture = Picture::Shown;
    show_picture();
    lv_obj_invalidate(s_photo);
}

void show_radar_details(const char *hex, const radar::Details &details)
{
    if (hex == nullptr || std::strcmp(hex, s_chosen) != 0) {
        return;  // the tap has moved on
    }
    s_details = details;
    std::snprintf(s_details_hex, sizeof(s_details_hex), "%s", hex);
    s_picture = !details.photo_checked          ? Picture::Loading
                : details.photo_url[0] != '\0' ? Picture::Loading
                                                : Picture::Missing;
    show_radar(*s_last);
}

}  // namespace ui
