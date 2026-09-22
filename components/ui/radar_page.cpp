#include "radar_page.h"

#include "esp_heap_caps.h"
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
// Room outside the outer ring for the compass letters. It has to hold the gap
// plus a whole line box: at 24 the N and S boxes landed two pixels outside the
// scope itself, so they were clipped against the ring rather than sitting clear
// of it.
constexpr int MARK = 36;

// Four, not twelve: at this size a spoke every thirty degrees is a texture
// rather than a reference, and it was most of what made the scope look busy.
constexpr int SPOKES = 4;
constexpr int RINGS  = 4;

constexpr int BOX = 24;  // local frame each outline is drawn in

// Aircraft out past the edge are still worth knowing about, so they sit on the
// rim at their bearing and slide inwards as they close.
constexpr int RIM_DOTS = 14;
constexpr int RIM_SIZE = 6;

struct Blip;

// Distance, altitude, speed, track, squawk. Not bearing: the whole purpose of
// drawing a scope is that you can see the bearing.
constexpr int READINGS = 5;

// Coastline, borders and water, drawn once when Home Assistant says where home
// is. A pool rather than an object per path: the embedded map covers several
// hundred kilometres and only the part inside the outer ring is ever wanted,
// which is a few dozen runs of it. The vertices live in PSRAM, where several
// thousand of them are not missed.
// The longest single path the generator emits, projected one at a time.
constexpr int MAP_POINTS     = 2400;
constexpr int MASK_MAX_EDGES = 64;

// Water is the one thing on this screen allowed to be cold: everything else is
// warm on near-black, and a coastline that reads as the edge of the sea tells
// you more than another cream line would.
constexpr std::uint32_t INK_WATER = 0x5d93b8;

// The steps the two buttons walk through, in kilometres.
// Lakes are filled rather than outlined, which LVGL has no primitive for, so
// they are scanline-filled into a canvas laid under the scope. One canvas for
// all of them, the size of the scope, in PSRAM.

constexpr int RANGES[]     = {20, 40, 60, 80, 100, 120, 140, 160};
constexpr int RANGE_COUNT  = static_cast<int>(std::size(RANGES));
constexpr int RANGE_BUTTON = 72;

// Long enough to read as movement, short enough that a second press is not
// waiting on it. The rings never move -- they are fixed fractions of whatever
// the range is -- so only the ground and the aircraft travel.
constexpr std::uint32_t ZOOM_MS = 260;

// A degree of latitude is 110.57 km and a degree of longitude is 111.32 km
// times the cosine of the latitude. Flat earth over eighty kilometres is off by
// less than the width of the line being drawn.
constexpr float KM_PER_LAT = 110.574f;
constexpr float KM_PER_LON = 111.320f;

// The altitude scale in the scope's bottom-left corner.
constexpr int          SCALE_TOP_FT = 40000;
constexpr int          LEGEND_STEPS = 18;
constexpr std::int32_t LEGEND_W     = 108;
constexpr std::int32_t LEGEND_H     = 8;

constexpr float KM_PER_NM = 1.852f;

// The column beside the scope, top to bottom. No card behind it and no boxes
// around the readings: a second surface sitting next to a circle read as
// something bolted on rather than part of the same page.
constexpr std::int32_t COLUMN_W   = 294;
constexpr std::int32_t COLUMN_GAP = 28;
constexpr std::int32_t ROW_H      = 30;
constexpr std::int32_t ROWS_Y     = 206;
constexpr std::int32_t FOOT_H     = 22;

struct Blip {
    lv_obj_t          *shape;
    lv_obj_t          *dot;
    lv_obj_t          *label;
    lv_point_precise_t points[8];
};

// An outline says which way it is pointing and what sort of thing it is, but
// its middle is empty, so where the aircraft actually is was a guess. A filled
// dot underneath answers that exactly.
constexpr int DOT_SIZE = 5;

// The emitter class an aircraft broadcasts about itself is enough to tell a
// helicopter from a jumbo without asking anybody, and a scope on which all of
// them are the same triangle is throwing that away.
enum class Shape : std::uint8_t { Airliner, Heavy, Light, Rotor, Other };

// Nose towards negative y, which is up before the track rotation is applied.
constexpr float OUTLINE_AIRLINER[][2] = {{0, -9}, {7, 4}, {0, 1}, {-7, 4}};
constexpr float OUTLINE_HEAVY[][2]    = {{0, -11}, {10, 5}, {0, 1}, {-10, 5}};
constexpr float OUTLINE_LIGHT[][2]    = {{0, -6}, {4, 5}, {-4, 5}};
constexpr float OUTLINE_OTHER[][2]    = {{0, -6}, {6, 0}, {0, 6}, {-6, 0}};
// One stroke through both rotor diameters, because a line is a single polyline.
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
// While a zoom runs, the range the scope is drawn at slides from the old value
// to the new one and the two ground pictures are scaled rather than redrawn;
// rasterising them every frame would cost far more than it is worth.
float       s_shown_range   = 0.0f;
int         s_zoom_from     = 0;
lv_obj_t   *s_zoom_in       = nullptr;
lv_obj_t   *s_zoom_out      = nullptr;
int         s_range_step    = 3;  // 80 km, which is where it starts

// The last of the per-aircraft cost to leave the internal pool. With these in
// PSRAM, raising how many aircraft the scope keeps costs it nothing.
Blip              *s_blips = nullptr;
lv_obj_t          *s_rim[RIM_DOTS]              = {};
lv_obj_t          *s_legend[LEGEND_STEPS]       = {};

// Water is an area, so it is filled rather than outlined, and LVGL has no
// polygon primitive. Everything wet is scanline-filled into one alpha mask --
// one byte a pixel, one image object, one colour from the object's recolour --
// instead of a line object per path. A full ARGB layer the size of the scope
// was what took the internal heap to nothing and brought down the Wi-Fi
// co-processor's SDIO driver, which asserts when it cannot get a buffer.
constexpr std::uint8_t WATER_ALPHA = 0x5c;

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

// The frame is shrunk to whatever the picture actually fills, because corners
// can only be cut off something that reaches them: rounding a frame wider than
// the picture inside it rounds empty space.
std::int32_t s_photo_box_y = 0;
std::int32_t s_photo_box_h = 0;
lv_obj_t *s_nearby     = nullptr;

lv_image_dsc_t s_photo_dsc     = {};
Row            s_rows[READINGS] = {};

// Held against the selection so a lookup that comes back after the tap has
// moved on is thrown away rather than shown against the wrong aircraft.
radar::Details s_details                     = {};
char           s_details_hex[radar::kHexLen] = {};

// Held so the panel can be redrawn when the selection changes rather than only
// when a reading lands.
// In PSRAM: a Snapshot carries the whole aircraft list, and this is the fourth
// copy of it. None of it is touched from an interrupt or with the cache off.
radar::Snapshot *s_last = nullptr;
char            s_chosen[radar::kHexLen] = {};

// Until the page is tapped it follows the nearest aircraft on its own, so
// arriving at it never shows an empty column. From the first tap onwards the
// selection is exactly what was tapped, and tapping open sky means nothing --
// following the nearest one anyway would be the panel overruling the tap.
bool s_following = true;

// A picture is two requests behind the tap that asked for it, so the wait gets
// said out loud rather than leaving a gap where one is about to appear.
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

// Height by brightness within whatever the accent colour happens to be, rather
// than by hue. The map projects use a full rainbow, and borrowing it put cyan
// and violet on a screen that is otherwise one warm colour on near-black:
// readable, and wrong. Built from the accent at each call so that choosing a
// different one takes the scope with it.
constexpr int ALTITUDE_STOPS[] = {0, 12000, 25000, 40000};

// Not on the scale: an aircraft that has not said how high it is.
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

// 7500 is a hijacking, 7600 a dead radio and 7700 everything else. Rare enough
// that nobody would look for them, which is exactly why they get shouted about.
const char *emergency(int squawk)
{
    switch (squawk) {
        case 7500: return "HIJACK";
        case 7600: return "NO RADIO";
        case 7700: return "EMERGENCY";
        default: return nullptr;
    }
}

// Measured rather than assumed: lv_obj_get_width only answers once a layout
// pass has run, and every placement below has to be settled before that.
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

// Off the letter's own measured box rather than a guess at how wide a glyph is.
// Assuming twelve by eighteen for all four put W and S through the outer ring
// and left E clear of it, which is the sort of thing that only looks like a
// design decision.
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
        lv_obj_set_style_line_color(spoke, lv_color_hex(theme::secondary), 0);
        lv_obj_set_style_line_opa(spoke, LV_OPA_10, 0);
        quiet(spoke);
    }

    // Brightest at the edge, so the outline of the scope reads first and the
    // inner rings stay reference rather than decoration.
    constexpr lv_opa_t WEIGHT[RINGS] = {LV_OPA_10, LV_OPA_20, LV_OPA_30, LV_OPA_40};
    for (int i = 1; i <= RINGS; ++i) {
        const std::int32_t radius = s_radius * i / RINGS;
        ring(scope, radius, WEIGHT[i - 1]);

        // On the north spoke alone: the same number four times over is three
        // more than anybody needs. Sitting above the ring rather than centred
        // on it, so the line does not run through the digits.
        char text[12];
        std::snprintf(text, sizeof(text), "%d", range_km * i / RINGS);
        lv_obj_t *label = theme::make_label(scope, text, theme::secondary, fonts::size_16());
        lv_obj_set_style_text_opa(label, LV_OPA_40, 0);
        lv_obj_set_pos(label, s_centre + 8, s_centre - radius - fonts::size_16()->line_height - 2);
        quiet(label);
        s_rings[i - 1] = label;
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

// The scope's background is clipped to a circle, so its square's bottom-left
// corner is space nothing can reach. The scale that reads the blips goes there,
// on the page behind it, rather than taking room in the column.
void build_legend(lv_obj_t *parent, std::int32_t side)
{
    const std::int32_t line = fonts::size_16()->line_height;
    // Sat against the bottom-left, with its whole box kept outside the outer
    // ring: the corner is only so big, and half the scale tucked under the
    // scope is worse than no scale at all.
    const std::int32_t x = 4;
    const std::int32_t y = side - line - LEGEND_H - 4;
    const std::int32_t step = LEGEND_W / LEGEND_STEPS;

    for (int i = 0; i < LEGEND_STEPS; ++i) {
        s_legend[i] = lv_obj_create(parent);
        lv_obj_set_size(s_legend[i], step + 1, LEGEND_H);
        lv_obj_set_pos(s_legend[i], x + i * step, y);
        theme::style_panel(s_legend[i], theme::panel, 0);
        quiet(s_legend[i]);
    }

    lv_obj_t *low = theme::make_label(parent, "0", theme::secondary, fonts::size_16());
    lv_obj_set_style_text_opa(low, LV_OPA_50, 0);
    lv_obj_set_pos(low, x, y + LEGEND_H + 2);
    quiet(low);

    lv_obj_t *high = theme::make_label(parent, "40 000 ft", theme::secondary, fonts::size_16());
    lv_obj_set_style_text_opa(high, LV_OPA_50, 0);
    lv_obj_set_pos(high, x + LEGEND_W - text_width("40 000 ft", fonts::size_16()),
                   y + LEGEND_H + 2);
    quiet(high);
}

// Everything the map is drawn from, and nothing about where the panel is: the
// data covers a region, and which part of it shows is decided here from
// whatever position arrives at runtime.
// The ground is two pictures, not a few dozen line objects. Everything wet goes
// into one alpha mask and every political line into another; each is a single
// image with its colour coming from the object's recolour. Drawn as objects it
// was forty-odd lines for LVGL to re-render whenever anything on the scope
// changed, which is what made opening the page take a visible moment and took
// the internal heap to nothing -- the Wi-Fi co-processor's SDIO driver asserts
// when it cannot get a receive buffer, and that is how the panel went down.
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

// Even-odd scanline fill, clipped to the outer ring. The polygon is whole --
// the generator clips areas with a polygon clipper so they stay closed -- so a
// lake half off the scope still fills the half that is on it.
// `rings` gives the length of each ring in `points`; the ones after the first
// are holes -- the islands in a sea -- and counting crossings across every ring
// at once is what leaves them unfilled.
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
        // The ring's half-width at this row, so staying inside the circle is
        // one square root a row rather than a test a pixel.
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

            // Kept far enough out that the rasteriser still draws the part
            // that crosses the scope, but not so far that the coordinates grow
            // silly. Every vertex is kept, so the ring lengths stay true.
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

// Greyed rather than merely inert, so the end of the range is visible before
// pressing rather than after.
void paint_range_buttons()
{
    if (s_zoom_in != nullptr) {
        s_range_step > 0 ? lv_obj_remove_state(s_zoom_in, LV_STATE_DISABLED)
                         : lv_obj_add_state(s_zoom_in, LV_STATE_DISABLED);
    }
    if (s_zoom_out != nullptr) {
        s_range_step < RANGE_COUNT - 1 ? lv_obj_remove_state(s_zoom_out, LV_STATE_DISABLED)
                                       : lv_obj_add_state(s_zoom_out, LV_STATE_DISABLED);
    }
}

// Nothing is fetched again. The sweep always asks for the farthest range the
// buttons go to and the page shows whichever of those are inside the one it is
// set to, so zooming is instant and never shows the wrong aircraft while a
// request for the new range is still in the air.
// The ground travels with the aircraft rather than fading: it was the full
// redraw on every frame that made the first attempt stutter, not the transform.
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

// A frame of a zoom moves what is already on the scope and nothing else. The
// full redraw sorts the aircraft, works out which tags can be shown without
// touching, and rewrites the column; none of that changes while the range
// slides, and doing it sixty times a second is what made the zoom stutter.
void move_blips(float range_km)
{
    for (int i = 0; i < s_shown; ++i) {
        // Zooming in takes the edge of the scope past aircraft that were inside
        // it, and they have to leave with it rather than carry on outwards
        // across the rest of the page.
        const float distance_km = s_plots[i].aircraft->distance_nm * KM_PER_NM;
        const bool  gone        = distance_km > range_km;
        lv_obj_set_hidden(s_blips[i].shape, gone);
        lv_obj_set_hidden(s_blips[i].dot, gone);
        lv_obj_set_hidden(s_blips[i].label, true);
        if (gone) {
            continue;
        }
        place_blip(s_plots[i], range_km);
        lv_obj_set_pos(s_blips[i].shape, s_plots[i].x - BOX / 2, s_plots[i].y - BOX / 2);
        lv_obj_set_pos(s_blips[i].dot, s_plots[i].x - DOT_SIZE / 2, s_plots[i].y - DOT_SIZE / 2);
    }
    lv_obj_set_hidden(s_marker, true);
}

void zoom_step(void *, std::int32_t value)
{
    const auto  from = static_cast<float>(s_zoom_from);
    const auto  to   = static_cast<float>(RANGES[s_range_step]);
    const float t    = static_cast<float>(value) / 256.0f;

    s_shown_range = from + (to - from) * t;
    // The ground was rasterised at the range the zoom started from, so it grows
    // by however much closer the scope has come since.
    scale_ground(from / s_shown_range);
    move_blips(s_shown_range);
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
    // Whatever is on screen right now, so pressing again mid-animation carries
    // on from where the scope has got to rather than jumping back.
    const int from = s_shown_range > 0.0f ? static_cast<int>(std::lround(s_shown_range))
                                          : RANGES[s_range_step];
    s_range_step   = next;
    apply_range(from);
}

// A button rather than a styled panel, so it grows under a finger the way every
// other button on the panel does; that comes from the theme, not from here.
lv_obj_t *build_range_button(lv_obj_t *parent, std::int32_t x, const char *text, int step)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, RANGE_BUTTON, RANGE_BUTTON);
    lv_obj_set_pos(button, x, 4);
    // The same helper the navigation tabs use, so these press and grow like
    // every other button rather than like a panel that happens to be tappable.
    theme::style_button(button, theme::panel_light);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme::disabled), LV_STATE_DISABLED);
    lv_obj_add_event_cb(button, range_clicked, LV_EVENT_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(step)));

    lv_obj_t *label = theme::make_label(button, text, theme::secondary, fonts::size_28());
    lv_obj_set_style_text_color(label, lv_color_hex(theme::text), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(label, lv_color_hex(theme::disabled_ink), LV_STATE_DISABLED);
    lv_obj_center(label);
    quiet(label);
    return button;
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

    // The ground goes in a layer of its own, made before the rings so that
    // everything in it is underneath them. Shuffling each line to the back
    // afterwards was doing the same job by hand, and doing it wrong: rivers
    // came out over the rings they should pass beneath.
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
    // Repainted with the rest of the scope rather than carrying a shared style,
    // since there is no border-coloured accent style and one object does not
    // justify adding one.
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

    // A fixed pool: rebuilding these on every reading would churn the heap
    // every ten seconds for the whole uptime.
    for (int i = 0; i < radar::kMaxAircraft; ++i) {
        Blip &blip = s_blips[i];
        blip.shape = lv_line_create(scope);
        lv_obj_set_style_line_width(blip.shape, 2, 0);
        lv_obj_set_style_line_rounded(blip.shape, true, 0);
        lv_obj_set_hidden(blip.shape, true);
        quiet(blip.shape);

        blip.dot = lv_obj_create(scope);
        lv_obj_set_size(blip.dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(blip.dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(blip.dot, 0, 0);
        lv_obj_set_style_pad_all(blip.dot, 0, 0);
        lv_obj_set_hidden(blip.dot, true);
        quiet(blip.dot);

        blip.label = theme::make_label(scope, "", theme::secondary, fonts::size_16());
        lv_obj_set_hidden(blip.label, true);
        quiet(blip.label);
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

    s_title = stacked(column, 0, theme::text, fonts::size_32());
    theme::ink_accent(s_title);
    s_operator  = stacked(column, 46, theme::text, fonts::size_20());
    s_airframe  = stacked(column, 76, theme::secondary, fonts::size_16());
    s_route     = stacked(column, 112, theme::text, fonts::size_28());
    s_cities    = stacked(column, 150, theme::secondary, fonts::size_16());

    lv_obj_t *rule = lv_obj_create(column);
    lv_obj_set_size(rule, COLUMN_W, 1);
    lv_obj_set_pos(rule, 0, ROWS_Y - 16);
    theme::style_panel(rule, theme::secondary, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_20, 0);
    quiet(rule);

    for (int i = 0; i < READINGS; ++i) {
        const std::int32_t y = ROWS_Y + i * ROW_H;

        s_rows[i].name = theme::make_label(column, "", theme::secondary, fonts::size_16());
        lv_obj_set_pos(s_rows[i].name, 0, y + 4);
        quiet(s_rows[i].name);

        s_rows[i].value = theme::make_label(column, "--", theme::text, fonts::size_22());
        lv_obj_set_pos(s_rows[i].value, 0, y);
        lv_obj_set_width(s_rows[i].value, COLUMN_W);
        lv_obj_set_style_text_align(s_rows[i].value, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(s_rows[i].value, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_height(s_rows[i].value, fonts::size_22()->line_height);
        quiet(s_rows[i].value);
    }

    const std::int32_t photo_y = ROWS_Y + READINGS * ROW_H + 16;
    const std::int32_t photo_h = height - photo_y - FOOT_H - 4;

    // An image draws its own content rather than drawing it as a child, so a
    // radius on the image itself rounds nothing. The corners have to be cut by
    // a parent that clips what it contains.
    s_photo_box_y = photo_y;
    s_photo_box_h = photo_h;

    s_photo_frame = lv_obj_create(column);
    lv_obj_set_pos(s_photo_frame, 0, photo_y);
    lv_obj_set_size(s_photo_frame, COLUMN_W, photo_h);
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

    // With nothing selected the picture's space would be a hole, so it holds
    // what is overhead instead. Which is also the answer to what to tap.
    s_nearby = theme::make_label(column, "", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_nearby, 0, photo_y);
    lv_obj_set_width(s_nearby, COLUMN_W);
    lv_obj_set_style_text_line_space(s_nearby, 8, 0);
    lv_obj_set_hidden(s_nearby, true);
    quiet(s_nearby);

    // Sits where the picture will be, so the wait has somewhere to be said.
    s_photo_note = theme::make_label(column, "", theme::secondary, fonts::size_16());
    lv_obj_set_style_text_opa(s_photo_note, LV_OPA_60, 0);
    lv_obj_set_pos(s_photo_note, 0, photo_y + photo_h / 2 - 10);
    lv_obj_set_width(s_photo_note, COLUMN_W);
    lv_obj_set_style_text_align(s_photo_note, LV_TEXT_ALIGN_CENTER, 0);
    quiet(s_photo_note);

    // Only ever says anything when something is wrong.
    s_summary = theme::make_label(column, "", theme::amber, fonts::size_16());
    lv_obj_set_width(s_summary, COLUMN_W);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_summary, fonts::size_16()->line_height);
    lv_obj_align(s_summary, LV_ALIGN_BOTTOM_LEFT, 0, 0);
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

    // Tags turned inwards: on the right of the scope they would otherwise run
    // off the edge, and two aircraft either side of the centre would have their
    // labels pointing at each other.
    const std::int32_t span = text_width(blip_name(*plot.aircraft), fonts::size_16());
    plot.label_x = plot.x > s_centre ? plot.x - 11 - span : plot.x + 11;
    plot.label_w = span;
}

void draw_blip(Blip &blip, const Plot &plot, bool named)
{
    // Heading is clockwise from north and the screen's y runs downwards, which
    // is what puts the minus on the sine of y rather than of x.
    const float track = plot.aircraft->track_deg < 0.0f ? 0.0f : plot.aircraft->track_deg;
    const float sin_t = std::sin(track * DEG);
    const float cos_t = std::cos(track * DEG);

    const Outline outline = outline_for(plot.aircraft->category);
    for (int i = 0; i < outline.count; ++i) {
        const float px = outline.points[i][0] * cos_t - outline.points[i][1] * sin_t;
        const float py = outline.points[i][0] * sin_t + outline.points[i][1] * cos_t;
        blip.points[i] = {static_cast<std::int32_t>(std::lround(px)) + BOX / 2,
                          static_cast<std::int32_t>(std::lround(py)) + BOX / 2};
    }
    int count = outline.count;
    if (outline.closed) {
        blip.points[count++] = blip.points[0];
    }

    const bool shout = emergency(plot.aircraft->squawk) != nullptr;

    lv_line_set_points(blip.shape, blip.points, static_cast<std::uint32_t>(count));
    lv_obj_set_pos(blip.shape, plot.x - BOX / 2, plot.y - BOX / 2);
    lv_obj_set_style_line_color(blip.shape, lv_color_hex(shout ? theme::red
                                                              : altitude_ink(
                                                                    plot.aircraft->altitude_ft)),
                                0);
    const std::uint32_t ink = shout ? theme::red : altitude_ink(plot.aircraft->altitude_ft);
    lv_obj_set_style_line_width(blip.shape, shout ? 3 : 2, 0);
    lv_obj_set_hidden(blip.shape, false);

    lv_obj_set_pos(blip.dot, plot.x - DOT_SIZE / 2, plot.y - DOT_SIZE / 2);
    theme::set_bg_color(blip.dot, ink);
    lv_obj_set_hidden(blip.dot, false);

    if (!named) {
        lv_obj_set_hidden(blip.label, true);
        return;
    }

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

// Nothing is selected, so the column says what is overhead rather than standing
// empty with the word Radar at the top of it.
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

    // The four nearest, where the picture would be.
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

    // Only what came back for this aircraft; the lookup is one flight behind
    // whenever a tap lands while the last one is still in the air.
    const bool mine = std::strcmp(s_details_hex, aircraft.hex) == 0;

    if (mine && s_details.airline[0] != '\0') {
        theme::set_text(s_operator, s_details.airline);
    } else if (mine && s_details.owner[0] != '\0') {
        theme::set_text(s_operator, s_details.owner);
    } else {
        theme::set_text(s_operator, "");
    }

    // Registration and shape on one line: three labels for three short strings
    // was most of the clutter. The feed sends no prose description, so who built
    // it and what model it is are put back together from the lookup, which
    // keeps them in separate fields.
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
        // Level, climbing or descending, at dump1090's threshold: below this a
        // rate is the aircraft breathing rather than going anywhere.
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
        heap_caps_calloc(radar::kMaxAircraft, sizeof(Blip), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_plots = static_cast<Plot *>(
        heap_caps_calloc(radar::kMaxAircraft, sizeof(Plot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_last == nullptr || s_blips == nullptr || s_plots == nullptr) {
        return;
    }

    const std::int32_t side = std::min(width - COLUMN_W - COLUMN_GAP, height);
    s_shown_range = static_cast<float>(RANGES[s_range_step]);
    build_scope(page, side, RANGES[s_range_step]);
    build_legend(page, side);
    s_zoom_out = build_range_button(page, 4, LV_SYMBOL_MINUS, 1);
    s_zoom_in  = build_range_button(page, side - RANGE_BUTTON - 4, LV_SYMBOL_PLUS, -1);
    paint_range_buttons();
    build_column(page, side + COLUMN_GAP, height);
    show_radar(*s_last);
}

void radar_page_opened()
{
    s_following = true;
}

void show_radar(const radar::Snapshot &snapshot)
{
    if (s_scope == nullptr || s_last == nullptr || s_blips == nullptr) {
        return;
    }
    if (&snapshot != s_last) {
        *s_last = snapshot;
    }

    // Wherever the zoom has got to, so the aircraft travel with the ground
    // instead of jumping to the new range while it is still moving.
    const float range_km = s_shown_range > 0.0f ? s_shown_range
                                                : static_cast<float>(RANGES[s_range_step]);

    // Only when the centre actually moves, which is once at startup.
    if (s_last->home_lat != s_map_lat || s_last->home_lon != s_map_lon) {
        s_map_lat = s_last->home_lat;
        s_map_lon = s_last->home_lon;
        if (s_map_lat != 0.0f || s_map_lon != 0.0f) {
            draw_map(s_map_lat, s_map_lon, range_km);
        }
    }

    s_shown      = 0;
    int rim_used = 0;
    for (int i = 0; i < s_last->count && s_shown < radar::kMaxAircraft; ++i) {
        const radar::Aircraft &aircraft = s_last->list[i];
        if (aircraft.on_ground) {
            continue;
        }
        const float distance_km = aircraft.distance_nm * KM_PER_NM;
        if (distance_km > range_km) {
            // Out past the edge, so it sits on the rim at its bearing instead
            // of vanishing: an empty scope and a quiet sky look the same.
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

    // The scale is built from the accent, so it has to be repainted when the
    // accent changes. set_bg_color skips a write that would change nothing, so
    // when it has not changed this costs nothing.
    for (int i = 0; i < LEGEND_STEPS; ++i) {
        theme::set_bg_color(s_legend[i], altitude_ink(SCALE_TOP_FT * i / (LEGEND_STEPS - 1)));
    }

    std::sort(s_plots, s_plots + s_shown, [](const Plot &a, const Plot &b) {
        return a.aircraft->distance_nm < b.aircraft->distance_nm;
    });

    // Nothing tapped yet, so the nearest aircraft is the subject. It is also the
    // one whose details have already been fetched, so this costs no network.
    if (s_following) {
        if (s_shown > 0) {
            // Two aircraft at much the same distance trade places every sweep,
            // and each swap threw away the picture that had just been fetched
            // for the other one. The one being followed keeps its place until
            // something is clearly nearer.
            int held = -1;
            for (int i = 0; i < s_shown; ++i) {
                if (std::strcmp(s_chosen, s_plots[i].aircraft->hex) == 0) {
                    held = i;
                    break;
                }
            }
            const bool keep = held >= 0 && s_plots[held].aircraft->distance_nm <
                                               s_plots[0].aircraft->distance_nm * 1.2f;
            if (!keep) {
                std::memcpy(s_chosen, s_plots[0].aircraft->hex, sizeof(s_chosen));
                s_chosen[sizeof(s_chosen) - 1] = '\0';
                if (std::strcmp(s_details_hex, s_chosen) != 0) {
                    s_picture = Picture::Looking;
                    radar::request_details(s_chosen, s_plots[0].aircraft->flight);
                }
            }
        } else {
            s_chosen[0] = '\0';
        }
    }

    for (int i = 0; i < s_shown; ++i) {
        place_blip(s_plots[i], range_km);
    }

    // Name everything that can be named without two tags touching. Where two
    // would collide neither is readable, so neither is drawn; the one that was
    // tapped is shown regardless, because that one was asked for.
    bool named[radar::kMaxAircraft];
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

    // The accent can change while the page is built, and this is the one thing
    // on the scope wearing it that no shared style reaches.
    const lv_color_t accent = lv_color_hex(theme::primary);
    if (!theme::has_local_color(s_marker, LV_STYLE_BORDER_COLOR, 0, accent)) {
        lv_obj_set_style_border_color(s_marker, accent, 0);
    }

    const radar::Aircraft *chosen = nullptr;
    for (int i = 0; i < radar::kMaxAircraft; ++i) {
        if (i >= s_shown) {
            lv_obj_set_hidden(s_blips[i].shape, true);
            lv_obj_set_hidden(s_blips[i].dot, true);
            lv_obj_set_hidden(s_blips[i].label, true);
            continue;
        }
        const bool mine =
            s_chosen[0] != '\0' && std::strcmp(s_chosen, s_plots[i].aircraft->hex) == 0;
        draw_blip(s_blips[i], s_plots[i], named[i] || mine);
        if (mine) {
            chosen = s_plots[i].aircraft;
            lv_obj_set_pos(s_marker, s_plots[i].x - 17, s_plots[i].y - 17);
            lv_obj_set_hidden(s_marker, false);
        }
    }

    if (chosen != nullptr) {
        show_selected(*chosen);
    } else {
        // Either nothing is selected or what was has left the scope.
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

    // Sized to what the picture will occupy once contained, and centred in the
    // space kept for it, so the rounded frame follows the picture's own edges.
    const std::int32_t fit_w = std::min(COLUMN_W, width * s_photo_box_h / height);
    const std::int32_t fit_h = std::min(s_photo_box_h, height * COLUMN_W / width);
    lv_obj_set_size(s_photo_frame, fit_w, fit_h);
    lv_obj_set_pos(s_photo_frame, (COLUMN_W - fit_w) / 2,
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
    // The picture is a second request behind this one, so the wait carries on
    // being shown rather than looking as though nothing is coming. Not knowing
    // of a picture is not the same as knowing there is none: details published
    // before the photo database has been asked would otherwise say there is no
    // photograph, and then one would appear a moment later.
    s_picture = !details.photo_checked          ? Picture::Loading
                : details.photo_url[0] != '\0' ? Picture::Loading
                                                : Picture::Missing;
    show_radar(*s_last);
}

}  // namespace ui
