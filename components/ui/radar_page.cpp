#include "radar_page.h"

#include "ui_internal.h"

#include "esp_heap_caps.h"
#include "map_data.h"
#include "theme.h"
#include "icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace ui {
namespace {
constexpr float DEG                    = 3.14159265f / 180.0f;
constexpr int   FULL_TURN_DEG          = 360;
constexpr float QUARTER_TURN_DEG       = 90.0f;
constexpr float ROTATION_UNITS_PER_DEG = 10.0f;

constexpr int           BITS_PER_CHANNEL = 8;
constexpr int           RED_SHIFT        = 2 * BITS_PER_CHANNEL;
constexpr std::uint32_t CHANNEL_MASK     = 0xff;

// LVGL stores RGB565A8 as an RGB565 plane followed by a separate alpha plane,
// not interleaved.
constexpr std::size_t RGB565_BYTES_PER_PX = sizeof(std::uint16_t);
constexpr std::size_t ALPHA_BYTES_PER_PX  = 1;
constexpr std::size_t AIR_BYTES_PER_PX    = RGB565_BYTES_PER_PX + ALPHA_BYTES_PER_PX;

constexpr int MIN_POLYGON_POINTS = 3;

// Must hold the gap plus a whole line box: at 24 the N and S labels landed
// outside the scope object and were clipped against the ring.
constexpr int RIM_BAND = 36;

constexpr int          SPOKES            = 8;
constexpr int          SPOKE_ENDS        = 2;
constexpr int          RINGS             = 4;
constexpr lv_opa_t     RING_OPA[RINGS]   = {LV_OPA_10, LV_OPA_20, LV_OPA_30, LV_OPA_40};
constexpr std::int32_t RING_STROKE       = 2;
constexpr std::int32_t RING_LABEL_PAD    = theme::space::xs;
constexpr float        RING_LABEL_ANGLE  = 45.0f * DEG;
constexpr std::size_t  RING_TEXT_LEN     = 12;
constexpr std::int32_t COMPASS_GAP       = 10;
constexpr std::int32_t HOME_SIZE         = 8;
constexpr std::int32_t MARKER_SIZE       = 34;
constexpr std::int32_t MARKER_STROKE     = 2;
constexpr int          RIM_DOTS          = 14;
constexpr int          RIM_SIZE          = 6;
constexpr std::int32_t TAP_REACH_DIVISOR = 10;

constexpr int          DRAWN_MAX          = radar::kMaxAircraft;
constexpr int          LABEL_MAX          = 20;
constexpr std::int32_t LABEL_SIDE_GAP     = 11;
constexpr std::int32_t LABEL_RISE         = 8;
constexpr std::int32_t LABEL_CLEARANCE    = 2;
constexpr int          OUTLINE_MAX_POINTS = 20;
constexpr int          BLIP_MAX_CROSSINGS = 12;
constexpr int          DOT_SIZE           = 5;

// Stays with the aircraft it follows until another is clearly nearer, so the
// card does not flick between two at much the same distance.
constexpr float FOLLOW_MARGIN = 1.2f;

constexpr int SQUAWK_HIJACK    = 7500;
constexpr int SQUAWK_NO_RADIO  = 7600;
constexpr int SQUAWK_EMERGENCY = 7700;

constexpr int   MAP_POINTS           = 2400;
constexpr int   MASK_MAX_EDGES       = 64;
constexpr float MICRODEGREES_PER_DEG = 1000000.0f;
constexpr float MAP_REACH_RADII      = 4.0f;

constexpr std::uint32_t INK_WATER      = 0x5d93b8;
constexpr std::uint8_t  WATER_ALPHA    = 0x5c;
constexpr std::uint8_t  LINE_ALPHA     = 0x70;
constexpr std::uint8_t  PROVINCE_ALPHA = 0x38;

constexpr int RANGES[]           = {20, 40, 60, 80, 100, 120, 140, 160};
constexpr int RANGE_COUNT        = static_cast<int>(std::size(RANGES));
constexpr int INITIAL_RANGE_STEP = 3;  // 80 km
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

constexpr float KEY_SEGMENT_OVERLAP_DEG = 0.6f;
constexpr float KEY_LABEL_GAP_DEG       = 2.0f;

constexpr std::int32_t SUMMARY_PAST_CORNER = 60;
constexpr std::int32_t SUMMARY_W           = CORNER + SUMMARY_PAST_CORNER;

constexpr std::uint32_t ZOOM_MS            = 260;
constexpr std::int32_t  ZOOM_PROGRESS_FULL = 256;

// Flat earth over eighty kilometres is off by less than the line width.
constexpr float KM_PER_LAT = 110.574f;
constexpr float KM_PER_LON = 111.320f;

constexpr float KM_PER_NM = 1.852f;

constexpr int           ALTITUDE_STOPS[]  = {0, 12000, 25000, 40000};
constexpr int           SCALE_TOP_FT      = ALTITUDE_STOPS[std::size(ALTITUDE_STOPS) - 1];
constexpr int           LEGEND_STEPS      = 18;
constexpr std::uint32_t INK_UNKNOWN       = 0x6b5a50;
constexpr float         UPPER_INK_TO_TEXT = 0.45f;
constexpr int           LEVEL_FLIGHT_FPM  = 245;

constexpr std::int32_t COLUMN_W   = 294;
constexpr std::int32_t COLUMN_GAP = theme::space::m;

// Both cards in the column are inset by the same amount, so the callsign, the
// readings and the photograph all start on one edge.
constexpr std::int32_t INSET          = detail::PANEL_PAD;
constexpr std::int32_t INNER_W        = COLUMN_W - 2 * INSET;
constexpr std::int32_t PHOTO_ASPECT_W = 3;
constexpr std::int32_t PHOTO_ASPECT_H = 2;
constexpr std::int32_t PHOTO_H        = INNER_W * PHOTO_ASPECT_H / PHOTO_ASPECT_W;

constexpr std::int32_t  SPINNER_SIZE      = 40;
constexpr std::int32_t  SPINNER_STROKE    = 4;
constexpr int           SPINNER_SWEEP_DEG = 90;
constexpr std::uint32_t SPINNER_TURN_MS   = 900;

constexpr int ALTITUDE_READING = 0;
constexpr int SPEED_READING    = 1;
constexpr int DISTANCE_READING = 2;
constexpr int READINGS         = 3;

constexpr std::int32_t READING_W = 78;
// The altitude carries a trend arrow, so it gets the wider column.
constexpr std::int32_t READING_WIDTHS[READINGS] = {INNER_W - (READINGS - 1) * READING_W,
                                                   READING_W, READING_W};

constexpr const char *NO_READING = "--";

constexpr int         NEARBY_LISTED     = 5;
constexpr std::size_t SUMMARY_TEXT_LEN  = 128;
constexpr std::size_t SELECTED_TEXT_LEN = 96;
constexpr std::size_t AIRFRAME_TEXT_LEN = 56;

enum class Shape : std::uint8_t { Airliner, Heavy, Light, Rotor, Other };

// Seen from above, nose towards negative y before the track turns it.
constexpr float OUTLINE_AIRLINER[][2] = {
    {0, -9},   {1.4f, -7}, {1.4f, -2}, {8, 2.5f},  {8, 4},     {1.4f, 2},
    {1.2f, 6}, {3.5f, 8},  {3.5f, 9},  {0, 8},     {-3.5f, 9}, {-3.5f, 8},
    {-1.2f, 6}, {-1.4f, 2}, {-8, 4},   {-8, 2.5f}, {-1.4f, -2}, {-1.4f, -7}};
constexpr float OUTLINE_HEAVY[][2] = {
    {0, -11.5f}, {1.8f, -9},  {1.8f, -2.5f}, {10, 3},  {10, 5},      {1.8f, 2.5f},
    {1.5f, 7.5f}, {4.5f, 10}, {4.5f, 11},    {0, 10},  {-4.5f, 11},  {-4.5f, 10},
    {-1.5f, 7.5f}, {-1.8f, 2.5f}, {-10, 5},  {-10, 3}, {-1.8f, -2.5f}, {-1.8f, -9}};
constexpr float OUTLINE_LIGHT[][2] = {
    {0, -6},  {1, -5},     {1, -2},   {6.5f, -1.5f}, {6.5f, 0.5f}, {1, 0.5f},
    {0.8f, 4}, {2.8f, 5},  {2.8f, 6}, {-2.8f, 6},    {-2.8f, 5},   {-0.8f, 4},
    {-1, 0.5f}, {-6.5f, 0.5f}, {-6.5f, -1.5f}, {-1, -2}, {-1, -5}};
constexpr float OUTLINE_OTHER[][2]    = {{0, -6}, {6, 0}, {0, 6}, {-6, 0}};
// One stroke through both diameters, because a line is a single polyline.
constexpr float OUTLINE_ROTOR[][2] = {{-7, -7}, {7, 7}, {0, 0}, {7, -7}, {-7, 7}};

struct Outline {
    const float (*points)[2];
    int  count;
    bool closed;
    bool dotted = false;  // a mark at the centre, for the shapes that are not a plane
};

struct Blip {
    lv_obj_t *label;
};

struct Row {
    lv_obj_t *name;
    lv_obj_t *value;
};

struct Plot {
    const radar::Aircraft *aircraft;
    float                  east_km;
    float                  north_km;
    std::int32_t           x;
    std::int32_t           y;
    std::int32_t           label_x;
    std::int32_t           label_w;
};

enum class Picture : std::uint8_t { Idle, Looking, Loading, Missing, Shown };

// Every reading redraws the whole scope, a 290 KB clear and every blip; not
// while another page is up. Opening the page catches up.
bool s_radar_stale = false;

lv_obj_t          *s_scope                      = nullptr;
lv_obj_t          *s_marker                     = nullptr;
lv_obj_t          *s_rings[RINGS]               = {};
lv_obj_t          *s_rim[RIM_DOTS]              = {};
lv_obj_t          *s_legend[LEGEND_STEPS]       = {};
lv_point_precise_t s_spokes[SPOKES][SPOKE_ENDS] = {};
std::int32_t       s_centre                     = 0;
std::int32_t       s_radius                     = 0;

float     s_shown_range = 0.0f;
int       s_zoom_from   = 0;
lv_obj_t *s_zoom_in     = nullptr;
lv_obj_t *s_zoom_out    = nullptr;
int       s_range_step  = INITIAL_RANGE_STEP;

lv_obj_t     *s_water_canvas = nullptr;
lv_obj_t     *s_land_canvas  = nullptr;
std::uint8_t *s_water_mask   = nullptr;
std::uint8_t *s_land_mask    = nullptr;
std::int32_t  s_ground_side  = 0;

lv_point_precise_t *s_map_points = nullptr;
float               s_map_lat    = 0.0f;
float               s_map_lon    = 0.0f;

lv_obj_t     *s_air_canvas = nullptr;
std::uint8_t *s_air_mask   = nullptr;

Blip *s_blips = nullptr;
Plot *s_plots = nullptr;
int   s_shown = 0;

lv_obj_t *s_title       = nullptr;
lv_obj_t *s_operator    = nullptr;
lv_obj_t *s_airframe    = nullptr;
lv_obj_t *s_route       = nullptr;
lv_obj_t *s_cities      = nullptr;
lv_obj_t *s_summary     = nullptr;
lv_obj_t *s_photo       = nullptr;
lv_obj_t *s_photo_frame = nullptr;
lv_obj_t *s_photo_wait  = nullptr;
lv_obj_t *s_photo_none  = nullptr;
lv_obj_t *s_identity    = nullptr;
lv_obj_t *s_nearby      = nullptr;

lv_image_dsc_t s_photo_dsc      = {};
Row            s_rows[READINGS] = {};

radar::Snapshot *s_last                        = nullptr;
char             s_chosen[radar::kHexLen]      = {};
bool             s_following                   = true;
radar::Details   s_details                     = {};
char             s_details_hex[radar::kHexLen] = {};
Picture          s_picture                     = Picture::Idle;

void ask_details(const char *hex, const char *callsign)
{
    if (detail::s_handlers.details != nullptr) {
        detail::s_handlers.details(hex, callsign);
    }
}

const lv_font_t *marking_font()
{
    return fonts::size_16();
}

template <typename T>
T *psram_array(std::size_t count)
{
    return static_cast<T *>(
        heap_caps_calloc(count, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

std::size_t ground_pixels()
{
    return static_cast<std::size_t>(s_ground_side) * s_ground_side;
}

// The ADS-B emitter category: A1 light, A4 high vortex and A5 heavy, A7 rotorcraft.
Shape shape_for(const char *category)
{
    if (category[0] != 'A') {
        return Shape::Other;
    }
    switch (category[1]) {
        case '1': return Shape::Light;
        case '4':
        case '5': return Shape::Heavy;
        case '7': return Shape::Rotor;
        default: return Shape::Airliner;
    }
}

template <std::size_t N>
Outline outline_of(const float (&points)[N][2], bool closed, bool dotted = false)
{
    return {points, static_cast<int>(N), closed, dotted};
}

Outline outline_for(const char *category)
{
    switch (shape_for(category)) {
        case Shape::Airliner: return outline_of(OUTLINE_AIRLINER, true);
        case Shape::Heavy: return outline_of(OUTLINE_HEAVY, true);
        case Shape::Light: return outline_of(OUTLINE_LIGHT, true);
        case Shape::Rotor: return outline_of(OUTLINE_ROTOR, false, true);
        default: return outline_of(OUTLINE_OTHER, true, true);
    }
}

std::uint32_t mix(std::uint32_t from, std::uint32_t to, float fraction)
{
    std::uint32_t blended = 0;
    for (int shift = RED_SHIFT; shift >= 0; shift -= BITS_PER_CHANNEL) {
        const auto  a     = static_cast<float>((from >> shift) & CHANNEL_MASK);
        const auto  b     = static_cast<float>((to >> shift) & CHANNEL_MASK);
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
        mix(theme::primary, theme::text, UPPER_INK_TO_TEXT),
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
        case SQUAWK_HIJACK: return "HIJACK";
        case SQUAWK_NO_RADIO: return "NO RADIO";
        case SQUAWK_EMERGENCY: return "EMERGENCY";
        default: return nullptr;
    }
}

float distance_km(const radar::Aircraft &aircraft)
{
    return aircraft.distance_nm * KM_PER_NM;
}

const char *blip_name(const radar::Aircraft &aircraft)
{
    return aircraft.flight[0] != '\0' ? aircraft.flight : aircraft.hex;
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

lv_obj_t *make_layer(lv_obj_t *parent, std::int32_t side)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_pos(layer, 0, 0);
    lv_obj_set_size(layer, side, side);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    quiet(layer);
    return layer;
}

lv_point_t on_circle(float angle, float radius)
{
    return {static_cast<std::int32_t>(s_centre + std::sin(angle) * radius),
            static_cast<std::int32_t>(s_centre - std::cos(angle) * radius)};
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
    lv_obj_set_style_border_width(circle, RING_STROKE, 0);
    lv_obj_set_style_pad_all(circle, 0, 0);
    quiet(circle);
}

void compass(lv_obj_t *parent, const char *text, char side)
{
    lv_obj_t          *label = theme::make_label(parent, text, theme::secondary, marking_font());
    const std::int32_t w     = text_width(text, marking_font());
    const std::int32_t h     = marking_font()->line_height;
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

void place_ring_label(lv_obj_t *label, std::int32_t radius)
{
    const std::int32_t w =
        text_width(lv_label_get_text(label), marking_font()) + 2 * RING_LABEL_PAD;
    const std::int32_t h  = marking_font()->line_height;
    const lv_point_t   at = on_circle(RING_LABEL_ANGLE, static_cast<float>(radius));
    lv_obj_set_pos(label, at.x - w / 2, at.y - h / 2);
}

void show_ring_distance(lv_obj_t *label, int nth, int range_km)
{
    char text[RING_TEXT_LEN];
    std::snprintf(text, sizeof(text), "%d", range_km * nth / RINGS);
    theme::set_text(label, text);
    place_ring_label(label, s_radius * nth / RINGS);
}

void build_spokes(lv_obj_t *scope)
{
    const auto inner = static_cast<float>(s_radius) / RINGS;
    const auto outer = static_cast<float>(s_radius);
    for (int i = 0; i < SPOKES; ++i) {
        const float      angle = static_cast<float>(i) * FULL_TURN_DEG / SPOKES * DEG;
        const lv_point_t from  = on_circle(angle, inner);
        const lv_point_t to    = on_circle(angle, outer);
        s_spokes[i][0]         = {from.x, from.y};
        s_spokes[i][1]         = {to.x, to.y};

        lv_obj_t *spoke = lv_line_create(scope);
        lv_line_set_points(spoke, s_spokes[i], SPOKE_ENDS);
        lv_obj_set_pos(spoke, 0, 0);
        lv_obj_set_style_line_width(spoke, 1, 0);
        lv_obj_set_style_line_color(spoke, lv_color_hex(theme::secondary), 0);
        lv_obj_set_style_line_opa(spoke, LV_OPA_10, 0);
        quiet(spoke);
    }
}

void build_rings(lv_obj_t *scope, int range_km)
{
    for (int i = 1; i <= RINGS; ++i) {
        ring(scope, s_radius * i / RINGS, RING_OPA[i - 1]);

        lv_obj_t *label = theme::make_label(scope, "", theme::secondary, marking_font());
        lv_obj_set_style_text_opa(label, LV_OPA_40, 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(theme::background), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(label, RING_LABEL_PAD, 0);
        show_ring_distance(label, i, range_km);
        quiet(label);
        s_rings[i - 1] = label;
    }
}

int plot_near(std::int32_t x, std::int32_t y)
{
    const std::int32_t reach   = lv_obj_get_width(s_scope) / TAP_REACH_DIVISOR;
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
    return best;
}

void remember_chosen(const radar::Aircraft &aircraft)
{
    std::memcpy(s_chosen, aircraft.hex, sizeof(s_chosen));
    s_chosen[sizeof(s_chosen) - 1] = '\0';
}

void forget_details()
{
    s_details        = radar::Details{};
    s_details_hex[0] = '\0';
}

void choose(const radar::Aircraft &aircraft)
{
    remember_chosen(aircraft);
    if (std::strcmp(s_details_hex, s_chosen) != 0) {
        forget_details();
        s_picture = Picture::Looking;
        ask_details(s_chosen, aircraft.flight);
    }
}

void choose_none()
{
    s_chosen[0] = '\0';
    forget_details();
    s_picture = Picture::Idle;
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
    const int best = plot_near(point.x - area.x1, point.y - area.y1);

    s_following = false;
    if (best < 0) {
        choose_none();
    } else {
        choose(*s_plots[best].aircraft);
    }
    show_radar(*s_last);
}

void build_key_scale(lv_obj_t *scope, std::int32_t mid)
{
    const std::int32_t outer = mid + KEY_WIDTH / 2;
    const float        step  = (KEY_TO - KEY_FROM) / LEGEND_STEPS;

    for (int i = 0; i < LEGEND_STEPS; ++i) {
        lv_obj_t *seg = lv_arc_create(scope);
        lv_obj_set_size(seg, 2 * outer, 2 * outer);
        lv_obj_set_pos(seg, s_centre - outer, s_centre - outer);
        lv_arc_set_bg_angles(
            seg, static_cast<lv_value_precise_t>(KEY_TO - (i + 1) * step - KEY_SEGMENT_OVERLAP_DEG),
            static_cast<lv_value_precise_t>(KEY_TO - i * step));
        lv_obj_set_style_arc_width(seg, KEY_WIDTH, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(seg, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(seg, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(seg, 0, LV_PART_KNOB);
        quiet(seg);
        s_legend[i] = seg;
    }
}

// Set just past each end of the arc, on the same circle, turned to follow it.
void build_key_end(lv_obj_t *scope, const char *text, float end_deg, float direction,
                   std::int32_t mid)
{
    const std::int32_t w     = text_width(text, marking_font());
    const std::int32_t h     = marking_font()->line_height;
    const float        half  = static_cast<float>(w) / 2.0f / static_cast<float>(mid) / DEG;
    const float        angle = end_deg + direction * (half + KEY_LABEL_GAP_DEG);
    const float        rad   = angle * DEG;
    lv_obj_t *label = theme::make_label(scope, text, theme::secondary, marking_font());
    lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
    lv_obj_set_pos(label, s_centre + static_cast<std::int32_t>(std::cos(rad) * mid) - w / 2,
                   s_centre + static_cast<std::int32_t>(std::sin(rad) * mid) - h / 2);
    lv_obj_set_style_transform_pivot_x(label, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(label, h / 2, 0);
    lv_obj_set_style_transform_rotation(
        label, static_cast<std::int32_t>((angle - QUARTER_TURN_DEG) * ROTATION_UNITS_PER_DEG), 0);
    quiet(label);
}

// The altitude scale runs along the rim band, the ring outside the last range
// ring where N, S, E and W already sit -- part of the scope rather than a key
// beside it. 0 at the steep end, 40k along the flatter bottom where a longer
// label lies easily, and the labels continue the arc instead of standing apart.
void build_key(lv_obj_t *scope)
{
    // Centred in the rim band on the same circle as the compass letters, so the
    // scale and N, S, E, W read as one set of markings.
    const std::int32_t mid = s_radius + COMPASS_GAP + marking_font()->line_height / 2;
    build_key_scale(scope, mid);
    build_key_end(scope, "0", KEY_TO, 1.0f, mid);
    build_key_end(scope, "40k ft", KEY_FROM, -1.0f, mid);
}

bool within_scope(std::int32_t x, std::int32_t y)
{
    const float limit = static_cast<float>(s_radius) * static_cast<float>(s_radius);
    const float ox    = static_cast<float>(x - s_centre);
    const float oy    = static_cast<float>(y - s_centre);
    return ox * ox + oy * oy <= limit && x >= 0 && x < s_ground_side && y >= 0 &&
           y < s_ground_side;
}

void mask_line(std::uint8_t *mask, std::int32_t x0, std::int32_t y0, std::int32_t x1,
               std::int32_t y1, std::uint8_t value)
{
    const std::int32_t dx     = std::abs(x1 - x0);
    const std::int32_t dy     = -std::abs(y1 - y0);
    const std::int32_t step_x = x0 < x1 ? 1 : -1;
    const std::int32_t step_y = y0 < y1 ? 1 : -1;
    std::int32_t       error  = dx + dy;

    for (;;) {
        if (within_scope(x0, y0)) {
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

struct Rows {
    std::int32_t top;
    std::int32_t bottom;
};

Rows rows_spanned(const lv_point_precise_t *points, int count)
{
    Rows rows = {points[0].y, points[0].y};
    for (int i = 1; i < count; ++i) {
        rows.top    = std::min<std::int32_t>(rows.top, points[i].y);
        rows.bottom = std::max<std::int32_t>(rows.bottom, points[i].y);
    }
    return rows;
}

// Appends where row y crosses the edges of one closed polygon.
int add_crossings(const lv_point_precise_t *polygon, int length, std::int32_t y,
                  std::int32_t *crossings, int found, int capacity)
{
    for (int i = 0, j = length - 1; i < length && found < capacity; j = i++) {
        const auto yi = static_cast<float>(polygon[i].y);
        const auto yj = static_cast<float>(polygon[j].y);
        if ((yi > static_cast<float>(y)) == (yj > static_cast<float>(y))) {
            continue;
        }
        const auto  xi = static_cast<float>(polygon[i].x);
        const auto  xj = static_cast<float>(polygon[j].x);
        const float t  = (static_cast<float>(y) - yi) / (yj - yi);
        crossings[found++] = static_cast<std::int32_t>(std::lround(xi + t * (xj - xi)));
    }
    return found;
}

void fill_water_row(std::int32_t y, const std::int32_t *crossings, int found)
{
    const float limit = static_cast<float>(s_radius) * static_cast<float>(s_radius);
    const float dy    = static_cast<float>(y - s_centre);
    const float span  = limit - dy * dy;
    if (span < 0.0f) {
        return;
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

void fill_water(const lv_point_precise_t *points, const std::uint16_t *rings, int ring_count,
                int count)
{
    if (s_water_mask == nullptr || count < MIN_POLYGON_POINTS) {
        return;
    }
    const Rows rows = rows_spanned(points, count);
    const std::int32_t top    = std::max<std::int32_t>(rows.top, s_centre - s_radius);
    const std::int32_t bottom = std::min<std::int32_t>(rows.bottom, s_centre + s_radius);

    for (std::int32_t y = top; y <= bottom; ++y) {
        std::int32_t crossings[MASK_MAX_EDGES];
        int          found = 0;
        int          base  = 0;
        for (int r = 0; r < ring_count; ++r) {
            found = add_crossings(points + base, rings[r], y, crossings, found, MASK_MAX_EDGES);
            base += rings[r];
        }
        std::sort(crossings, crossings + found);
        fill_water_row(y, crossings, found);
    }
}

struct Projection {
    float home_lat;
    float home_lon;
    float px_per_lat;
    float px_per_lon;
};

Projection projection_for(float home_lat, float home_lon, int range_km)
{
    const float scale = static_cast<float>(s_radius) / static_cast<float>(range_km);
    return {home_lat, home_lon, KM_PER_LAT * scale, KM_PER_LON * std::cos(home_lat * DEG) * scale};
}

lv_point_precise_t project(const std::int32_t *lat_lon, const Projection &projection)
{
    const auto  centre = static_cast<float>(s_centre);
    const float reach  = static_cast<float>(s_radius) * MAP_REACH_RADII;
    const float lat    = static_cast<float>(lat_lon[0]) / MICRODEGREES_PER_DEG;
    const float lon    = static_cast<float>(lat_lon[1]) / MICRODEGREES_PER_DEG;
    const float x      = centre + (lon - projection.home_lon) * projection.px_per_lon;
    const float y      = centre - (lat - projection.home_lat) * projection.px_per_lat;

    const float clamped_x = std::clamp(x, centre - reach, centre + reach);
    const float clamped_y = std::clamp(y, centre - reach, centre + reach);
    return {static_cast<std::int32_t>(std::lround(clamped_x)),
            static_cast<std::int32_t>(std::lround(clamped_y))};
}

int project_path(const radar::MapPath &path, const Projection &projection)
{
    int count = 0;
    for (int v = 0; v < path.count && count < MAP_POINTS; ++v) {
        s_map_points[count++] = project(path.points + v * 2, projection);
    }
    return count;
}

void stroke_path(radar::MapLayer layer, int count)
{
    std::uint8_t      *mask  = layer == radar::MapLayer::River ? s_water_mask : s_land_mask;
    const std::uint8_t value = layer == radar::MapLayer::Province ? PROVINCE_ALPHA : LINE_ALPHA;
    for (int v = 1; v < count; ++v) {
        mask_line(mask, s_map_points[v - 1].x, s_map_points[v - 1].y, s_map_points[v].x,
                  s_map_points[v].y, value);
    }
}

bool is_water_body(radar::MapLayer layer)
{
    return layer == radar::MapLayer::Ocean || layer == radar::MapLayer::Lake;
}

void draw_map(float home_lat, float home_lon, int range_km)
{
    if (s_water_mask == nullptr || s_land_mask == nullptr || s_map_points == nullptr) {
        return;
    }

    std::memset(s_water_mask, 0, ground_pixels());
    std::memset(s_land_mask, 0, ground_pixels());

    const Projection projection = projection_for(home_lat, home_lon, range_km);
    for (int i = 0; i < radar::kMapPathCount; ++i) {
        const radar::MapPath &path  = radar::kMapPaths[i];
        const int             count = project_path(path, projection);
        if (is_water_body(path.layer)) {
            fill_water(s_map_points, path.rings, path.ring_count, count);
        } else {
            stroke_path(path.layer, count);
        }
    }

    if (s_water_canvas != nullptr) {
        lv_obj_invalidate(s_water_canvas);
    }
    if (s_land_canvas != nullptr) {
        lv_obj_invalidate(s_land_canvas);
    }
}

bool map_located()
{
    return s_map_lat != 0.0f || s_map_lon != 0.0f;
}

void paint_range_buttons()
{
    theme::set_usable(s_zoom_in, s_range_step > 0);
    theme::set_usable(s_zoom_out, s_range_step < RANGE_COUNT - 1);
}

float shown_range_km()
{
    return s_shown_range > 0.0f ? s_shown_range : static_cast<float>(RANGES[s_range_step]);
}

std::uint32_t image_scale(float factor)
{
    return static_cast<std::uint32_t>(std::lround(factor * LV_SCALE_NONE));
}

void scale_ground(float factor)
{
    for (lv_obj_t *canvas : {s_water_canvas, s_land_canvas}) {
        if (canvas != nullptr) {
            lv_image_set_pivot(canvas, s_centre, s_centre);
            lv_image_set_scale(canvas, image_scale(factor));
        }
    }
}

void scale_traffic(float factor)
{
    if (s_air_canvas != nullptr) {
        lv_image_set_pivot(s_air_canvas, s_centre, s_centre);
        lv_image_set_scale(s_air_canvas, image_scale(factor));
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
    const float t    = static_cast<float>(value) / static_cast<float>(ZOOM_PROGRESS_FULL);

    s_shown_range      = from + (to - from) * t;
    const float factor = from / s_shown_range;
    scale_ground(factor);
    scale_traffic(factor);
}

void settle_zoom()
{
    const int settled = RANGES[s_range_step];
    s_shown_range     = static_cast<float>(settled);
    scale_ground(1.0f);
    if (map_located()) {
        draw_map(s_map_lat, s_map_lon, settled);
    }
    show_radar(*s_last);
}

void zoom_done(lv_anim_t *)
{
    settle_zoom();
}

void start_zoom(int from_km)
{
    s_zoom_from = from_km;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_scope);
    lv_anim_set_values(&anim, 0, ZOOM_PROGRESS_FULL);
    lv_anim_set_duration(&anim, ZOOM_MS);
    lv_anim_set_exec_cb(&anim, zoom_step);
    lv_anim_set_completed_cb(&anim, zoom_done);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);
}

void apply_range(int from_km)
{
    for (int i = 1; i <= RINGS; ++i) {
        show_ring_distance(s_rings[i - 1], i, RANGES[s_range_step]);
    }
    paint_range_buttons();

    if (from_km <= 0 || s_scope == nullptr) {
        settle_zoom();
        return;
    }
    start_zoom(from_km);
}

void range_clicked(lv_event_t *event)
{
    const auto step =
        static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(event)));
    const int next = std::clamp(s_range_step + step, 0, RANGE_COUNT - 1);
    if (next == s_range_step) {
        return;
    }
    const int from = static_cast<int>(std::lround(shown_range_km()));
    s_range_step   = next;
    apply_range(from);
}

// Round chips in the top corners, the same as the heating card's: a dot of the
// darker surface on the lighter card is what makes it read as something to
// press. The hit area reaches past the dot, so a finger does not have to find it.
lv_obj_t *zoom_chip(lv_obj_t *bezel, std::int32_t x, const lv_image_dsc_t *mark, int step)
{
    lv_obj_t *chip = theme::make_chip(bezel, "");
    theme::make_mark(chip, mark);
    lv_obj_set_pos(chip, x, EDGE);
    lv_obj_set_ext_click_area(chip, (CORNER - ZOOM_D) / 2);
    lv_obj_add_event_cb(chip, range_clicked, LV_EVENT_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(step)));
    return chip;
}

void build_zoom(lv_obj_t *bezel, std::int32_t side)
{
    s_zoom_out = zoom_chip(bezel, EDGE, &icons::minus_icon, 1);
    s_zoom_in  = zoom_chip(bezel, side - ZOOM_D - EDGE, &icons::plus_icon, -1);
}

lv_obj_t *mask_canvas(lv_obj_t *parent, std::uint8_t *mask, std::uint32_t ink)
{
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, mask, s_ground_side, s_ground_side, LV_COLOR_FORMAT_A8);
    lv_obj_set_style_image_recolor(canvas, lv_color_hex(ink), 0);
    lv_obj_set_style_image_recolor_opa(canvas, LV_OPA_COVER, 0);
    quiet(canvas);
    return canvas;
}

void build_ground(lv_obj_t *scope, std::int32_t side)
{
    lv_obj_t *ground = make_layer(scope, side);

    s_ground_side = side;
    s_water_mask  = static_cast<std::uint8_t *>(
        heap_caps_calloc(ground_pixels(), ALPHA_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    s_land_mask   = static_cast<std::uint8_t *>(
        heap_caps_calloc(ground_pixels(), ALPHA_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    s_map_points  = static_cast<lv_point_precise_t *>(heap_caps_malloc(
        sizeof(lv_point_precise_t) * MAP_POINTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (s_water_mask != nullptr && s_land_mask != nullptr) {
        s_water_canvas = mask_canvas(ground, s_water_mask, INK_WATER);
        s_land_canvas  = mask_canvas(ground, s_land_mask, theme::secondary);
    }
}

void build_home(lv_obj_t *scope)
{
    lv_obj_t *home = lv_obj_create(scope);
    lv_obj_set_size(home, HOME_SIZE, HOME_SIZE);
    lv_obj_set_pos(home, s_centre - HOME_SIZE / 2, s_centre - HOME_SIZE / 2);
    theme::style_panel(home, theme::panel, HOME_SIZE / 2);
    theme::fill_accent(home);
    quiet(home);
}

void build_marker(lv_obj_t *scope)
{
    s_marker = lv_obj_create(scope);
    lv_obj_set_size(s_marker, MARKER_SIZE, MARKER_SIZE);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_marker, lv_color_hex(theme::primary), 0);
    lv_obj_set_style_border_width(s_marker, MARKER_STROKE, 0);
    lv_obj_set_style_pad_all(s_marker, 0, 0);
    lv_obj_set_hidden(s_marker, true);
    quiet(s_marker);
}

void build_rim(lv_obj_t *scope)
{
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
}

void build_air(lv_obj_t *scope, std::int32_t side)
{
    s_air_mask = static_cast<std::uint8_t *>(
        heap_caps_calloc(ground_pixels(), AIR_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    if (s_air_mask != nullptr) {
        s_air_canvas = lv_canvas_create(scope);
        lv_canvas_set_buffer(s_air_canvas, s_air_mask, side, side, LV_COLOR_FORMAT_RGB565A8);
        lv_obj_set_pos(s_air_canvas, 0, 0);
        quiet(s_air_canvas);
    }
}

void build_blip_labels(lv_obj_t *scope)
{
    for (int i = 0; i < LABEL_MAX; ++i) {
        s_blips[i].label = theme::make_label(scope, "", theme::secondary, marking_font());
        lv_obj_set_hidden(s_blips[i].label, true);
        quiet(s_blips[i].label);
    }
}

void build_scope(lv_obj_t *parent, std::int32_t side, int range_km)
{
    s_centre = side / 2;
    s_radius = side / 2 - RIM_BAND;

    lv_obj_t *scope = lv_obj_create(parent);
    lv_obj_set_pos(scope, 0, 0);
    lv_obj_set_size(scope, side, side);
    theme::style_panel(scope, theme::background, side / 2);
    lv_obj_set_scrollable(scope, false);
    lv_obj_add_event_cb(scope, scope_clicked, LV_EVENT_CLICKED, nullptr);

    build_ground(scope, side);
    build_spokes(scope);
    build_rings(scope, range_km);
    compass(scope, "N", 'N');
    compass(scope, "S", 'S');
    compass(scope, "E", 'E');
    compass(scope, "W", 'W');
    build_home(scope);
    build_marker(scope);
    build_rim(scope);
    build_air(scope, side);
    build_blip_labels(scope);

    s_scope = scope;
}

void build_photo(lv_obj_t *card)
{
    // There from the tap on, so the text under it does not jump when it loads.
    s_photo_frame = lv_obj_create(card);
    lv_obj_set_pos(s_photo_frame, 0, 0);
    lv_obj_set_size(s_photo_frame, INNER_W, PHOTO_H);
    theme::style_panel(s_photo_frame, theme::panel, theme::radius::row);
    lv_obj_set_style_pad_all(s_photo_frame, 0, 0);
    lv_obj_set_style_clip_corner(s_photo_frame, true, 0);
    lv_obj_set_hidden(s_photo_frame, true);
    quiet(s_photo_frame);

    s_photo = lv_image_create(s_photo_frame);
    lv_obj_set_pos(s_photo, 0, 0);
    lv_image_set_inner_align(s_photo, LV_IMAGE_ALIGN_CONTAIN);
    quiet(s_photo);
}

// Not LVGL's spinner: its eased ends stutter whenever a frame is late. And
// outside the frame, whose clipped corners would force a layer per frame.
void build_spinner(lv_obj_t *card)
{
    s_photo_wait = lv_arc_create(card);
    lv_obj_set_size(s_photo_wait, SPINNER_SIZE, SPINNER_SIZE);
    lv_obj_set_pos(s_photo_wait, (INNER_W - SPINNER_SIZE) / 2, (PHOTO_H - SPINNER_SIZE) / 2);
    lv_arc_set_bg_angles(s_photo_wait, 0, FULL_TURN_DEG);
    lv_arc_set_angles(s_photo_wait, 0, SPINNER_SWEEP_DEG);
    lv_obj_remove_style(s_photo_wait, nullptr, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_photo_wait, 0, 0);
    lv_obj_set_style_arc_width(s_photo_wait, SPINNER_STROKE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_photo_wait, SPINNER_STROKE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_photo_wait, lv_color_hex(theme::panel_light), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_photo_wait, lv_color_hex(theme::secondary), LV_PART_INDICATOR);
    lv_obj_set_hidden(s_photo_wait, true);
    quiet(s_photo_wait);
}

void build_no_photo()
{
    s_photo_none = lv_image_create(s_photo_frame);
    lv_image_set_src(s_photo_none, &icons::no_photo_icon);
    lv_obj_set_style_image_recolor(s_photo_none, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_image_recolor_opa(s_photo_none, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(s_photo_none, LV_OPA_60, 0);
    lv_obj_center(s_photo_none);
    quiet(s_photo_none);
}

lv_obj_t *identity_line(std::uint32_t colour, const lv_font_t *font)
{
    lv_obj_t *label = theme::make_label(s_identity, "", colour, font);
    lv_obj_set_width(label, INNER_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, font->line_height);
    quiet(label);
    return label;
}

void build_identity(lv_obj_t *card)
{
    s_identity = lv_obj_create(card);
    lv_obj_set_size(s_identity, INNER_W, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_identity, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_identity, 0, 0);
    lv_obj_set_style_pad_all(s_identity, 0, 0);
    lv_obj_set_flex_flow(s_identity, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_identity, theme::space::xs, 0);
    quiet(s_identity);

    s_title = identity_line(theme::text, theme::type_title());
    theme::ink_accent(s_title);
    s_operator = identity_line(theme::text, theme::type_body());
    s_airframe = identity_line(theme::secondary, theme::type_label());
    s_route    = identity_line(theme::text, theme::type_value());
    lv_obj_set_style_margin_top(s_route, theme::space::m, 0);
    s_cities = identity_line(theme::secondary, theme::type_label());

    s_nearby = theme::make_label(s_identity, "", theme::secondary, theme::type_body());
    lv_obj_set_width(s_nearby, INNER_W);
    lv_obj_set_style_margin_top(s_nearby, theme::space::m, 0);
    lv_obj_set_style_text_line_space(s_nearby, theme::space::s, 0);
    lv_obj_set_hidden(s_nearby, true);
    quiet(s_nearby);
}

void build_readings(lv_obj_t *card, std::int32_t height)
{
    const std::int32_t name_y  = height - 2 * INSET - theme::type_label()->line_height;
    const std::int32_t value_y = name_y - theme::type_value()->line_height;
    std::int32_t       left    = 0;
    for (int i = 0; i < READINGS; ++i) {
        s_rows[i].value = theme::make_label(card, NO_READING, theme::text, theme::type_value());
        lv_obj_set_pos(s_rows[i].value, left, value_y);
        lv_obj_set_width(s_rows[i].value, READING_WIDTHS[i]);
        lv_label_set_long_mode(s_rows[i].value, LV_LABEL_LONG_MODE_CLIP);
        quiet(s_rows[i].value);

        s_rows[i].name = theme::make_label(card, "", theme::secondary, theme::type_label());
        lv_obj_set_pos(s_rows[i].name, left, name_y);
        quiet(s_rows[i].name);
        left += READING_WIDTHS[i];
    }
}

void build_column(lv_obj_t *parent, std::int32_t x, std::int32_t height)
{
    lv_obj_t *card = theme::make_card(parent);
    lv_obj_set_pos(card, x, 0);
    lv_obj_set_size(card, COLUMN_W, height);
    lv_obj_set_style_pad_all(card, INSET, 0);
    quiet(card);

    build_photo(card);
    build_spinner(card);
    build_no_photo();
    build_identity(card);
    build_readings(card, height);
}

void spin(void *arc, std::int32_t angle)
{
    lv_arc_set_rotation(static_cast<lv_obj_t *>(arc), angle);
}

void show_waiting(bool waiting)
{
    if (waiting == !lv_obj_is_hidden(s_photo_wait)) {
        return;
    }
    lv_obj_set_hidden(s_photo_wait, !waiting);
    if (!waiting) {
        lv_anim_delete(s_photo_wait, nullptr);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_photo_wait);
    lv_anim_set_exec_cb(&anim, spin);
    lv_anim_set_values(&anim, 0, FULL_TURN_DEG);
    lv_anim_set_duration(&anim, SPINNER_TURN_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

void show_picture()
{
    const bool shown = s_picture == Picture::Shown;
    if (!shown) {
        lv_obj_set_size(s_photo_frame, INNER_W, PHOTO_H);
    }
    lv_obj_set_hidden(s_photo_frame, s_picture == Picture::Idle);
    lv_obj_set_hidden(s_photo, !shown);
    show_waiting(s_picture == Picture::Looking || s_picture == Picture::Loading);
    lv_obj_set_hidden(s_photo_none, s_picture != Picture::Missing);
    lv_obj_set_y(s_identity, s_picture == Picture::Idle ? 0 : PHOTO_H + theme::space::m);
}

void place_blip(Plot &plot, float range_km)
{
    const float scale = static_cast<float>(s_radius) / range_km;
    plot.x = static_cast<std::int32_t>(std::lround(s_centre + plot.east_km * scale));
    plot.y = static_cast<std::int32_t>(std::lround(s_centre - plot.north_km * scale));

    const std::int32_t span = text_width(blip_name(*plot.aircraft), marking_font());
    plot.label_x = plot.x > s_centre ? plot.x - LABEL_SIDE_GAP - span : plot.x + LABEL_SIDE_GAP;
    plot.label_w = span;
}

std::uint8_t *air_alpha_plane()
{
    return s_air_mask + ground_pixels() * RGB565_BYTES_PER_PX;
}

void air_pixels(std::int32_t y, std::int32_t from, std::int32_t to, std::uint16_t ink)
{
    const std::size_t row    = static_cast<std::size_t>(y) * s_ground_side;
    auto             *colour = reinterpret_cast<std::uint16_t *>(s_air_mask) + row;
    std::uint8_t     *alpha  = air_alpha_plane() + row;
    std::fill(colour + from, colour + to + 1, ink);
    std::memset(alpha + from, LV_OPA_COVER, static_cast<std::size_t>(to - from + 1));
}

// By hand: std::sort over this small fixed buffer trips GCC's array-bounds check.
void sort_crossings(std::int32_t *crossings, int found)
{
    for (int i = 1; i < found; ++i) {
        const std::int32_t value = crossings[i];
        int                j     = i - 1;
        while (j >= 0 && crossings[j] > value) {
            crossings[j + 1] = crossings[j];
            --j;
        }
        crossings[j + 1] = value;
    }
}

void fill_blip(const lv_point_precise_t *points, int count, std::uint16_t ink)
{
    if (s_air_mask == nullptr || count < MIN_POLYGON_POINTS) {
        return;
    }
    const Rows         rows   = rows_spanned(points, count);
    const std::int32_t top    = std::max<std::int32_t>(rows.top, 0);
    const std::int32_t bottom = std::min<std::int32_t>(rows.bottom, s_ground_side - 1);

    for (std::int32_t y = top; y <= bottom; ++y) {
        std::int32_t crossings[BLIP_MAX_CROSSINGS];
        const int    found = add_crossings(points, count, y, crossings, 0, BLIP_MAX_CROSSINGS);
        sort_crossings(crossings, found);
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

void dot_blip(const Plot &plot, std::uint16_t ink)
{
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

void turn_outline(const Outline &outline, const Plot &plot, lv_point_precise_t *shape)
{
    const float track = plot.aircraft->track_deg < 0.0f ? 0.0f : plot.aircraft->track_deg;
    const float sin_t = std::sin(track * DEG);
    const float cos_t = std::cos(track * DEG);
    for (int i = 0; i < outline.count; ++i) {
        const float px = outline.points[i][0] * cos_t - outline.points[i][1] * sin_t;
        const float py = outline.points[i][0] * sin_t + outline.points[i][1] * cos_t;
        shape[i] = {plot.x + static_cast<std::int32_t>(std::lround(px)),
                    plot.y + static_cast<std::int32_t>(std::lround(py))};
    }
}

void raster_blip(const Plot &plot)
{
    const Outline      outline = outline_for(plot.aircraft->category);
    lv_point_precise_t shape[OUTLINE_MAX_POINTS];
    turn_outline(outline, plot, shape);

    const bool          shout = emergency(plot.aircraft->squawk) != nullptr;
    const std::uint16_t ink   = lv_color_to_u16(
        lv_color_hex(shout ? theme::red : altitude_ink(plot.aircraft->altitude_ft)));

    if (outline.closed) {
        fill_blip(shape, outline.count, ink);
    }
    if (outline.dotted) {
        dot_blip(plot, ink);
    }
}

void draw_label(Blip &blip, const Plot &plot)
{
    const bool shout = emergency(plot.aircraft->squawk) != nullptr;
    theme::set_text(blip.label, blip_name(*plot.aircraft));
    theme::set_text_color(blip.label, shout ? theme::red : theme::secondary);
    lv_obj_set_pos(blip.label, plot.label_x, plot.y - LABEL_RISE);
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

void set_row(int index, const char *name, const char *value, std::uint32_t colour = theme::text)
{
    theme::set_text(s_rows[index].name, name);
    theme::set_text(s_rows[index].value, value);
    theme::set_text_color(s_rows[index].value, colour);
}

void show_summary_heading()
{
    char text[SUMMARY_TEXT_LEN];
    std::snprintf(text, sizeof(text), "%d aircraft", s_shown);
    theme::set_text(s_title, s_shown > 0 ? text : "Quiet sky");
    theme::set_text(s_operator, s_shown > 0 ? "Tap one for its details" : "Nothing within range");
    theme::set_text(s_airframe, "");
    theme::set_text(s_route, "");
    theme::set_text(s_cities, "");
    lv_obj_set_hidden(s_route, true);
    lv_obj_set_hidden(s_cities, true);
    lv_obj_set_hidden(s_airframe, true);
}

void show_summary_readings()
{
    const radar::Aircraft *highest = nullptr;
    const radar::Aircraft *fastest = nullptr;
    for (int i = 0; i < s_shown; ++i) {
        const radar::Aircraft *a = s_plots[i].aircraft;
        if (a->altitude_ft >= 0 && (highest == nullptr || a->altitude_ft > highest->altitude_ft)) {
            highest = a;
        }
        if (fastest == nullptr || a->speed_kt > fastest->speed_kt) {
            fastest = a;
        }
    }

    char text[SUMMARY_TEXT_LEN];
    if (highest != nullptr) {
        std::snprintf(text, sizeof(text), "%d", highest->altitude_ft);
    }
    set_row(ALTITUDE_READING, "feet, top", highest != nullptr ? text : NO_READING);
    if (fastest != nullptr) {
        std::snprintf(text, sizeof(text), "%.0f", static_cast<double>(fastest->speed_kt));
    }
    set_row(SPEED_READING, "knots, top", fastest != nullptr ? text : NO_READING);
    if (s_shown > 0) {
        std::snprintf(text, sizeof(text), "%.1f",
                      static_cast<double>(distance_km(*s_plots[0].aircraft)));
    }
    set_row(DISTANCE_READING, "km, closest", s_shown > 0 ? text : NO_READING);
}

void show_nearby()
{
    char text[SUMMARY_TEXT_LEN];
    text[0]    = '\0';
    int length = 0;
    for (int i = 0; i < s_shown && i < NEARBY_LISTED; ++i) {
        const radar::Aircraft *a = s_plots[i].aircraft;
        length += std::snprintf(text + length, sizeof(text) - length, "%s%s  %.0f km",
                                i > 0 ? "\n" : "", blip_name(*a),
                                static_cast<double>(distance_km(*a)));
    }
    theme::set_text(s_nearby, text);
    lv_obj_set_hidden(s_nearby, s_shown == 0);
}

void show_summary(const radar::Snapshot &snapshot)
{
    show_summary_heading();
    show_summary_readings();
    show_nearby();

    s_picture = Picture::Idle;
    show_picture();

    theme::set_text(s_summary, snapshot.age_s < 0 ? "waiting for a fix"
                               : !snapshot.ok    ? "feed unreachable"
                                                 : "");
    theme::set_text_color(s_summary, theme::amber);
}

const char *operator_name(bool mine)
{
    if (mine && s_details.airline[0] != '\0') {
        return s_details.airline;
    }
    if (mine && s_details.owner[0] != '\0') {
        return s_details.owner;
    }
    return "";
}

void describe_shape(const radar::Aircraft &aircraft, bool mine, char *shape, std::size_t size)
{
    if (mine && s_details.manufacturer[0] != '\0' && s_details.model[0] != '\0') {
        std::snprintf(shape, size, "%s %s", s_details.manufacturer, s_details.model);
    } else if (aircraft.desc[0] != '\0') {
        std::snprintf(shape, size, "%s", aircraft.desc);
    } else if (mine && s_details.model[0] != '\0') {
        std::snprintf(shape, size, "%s", s_details.model);
    } else {
        std::snprintf(shape, size, "%s", aircraft.type);
    }
}

void show_airframe(const radar::Aircraft &aircraft, bool mine)
{
    char shape[AIRFRAME_TEXT_LEN] = {};
    describe_shape(aircraft, mine, shape, sizeof(shape));

    char text[SELECTED_TEXT_LEN];
    if (aircraft.reg[0] != '\0' && shape[0] != '\0') {
        std::snprintf(text, sizeof(text), "%s, %s", aircraft.reg, shape);
    } else {
        std::snprintf(text, sizeof(text), "%s",
                      aircraft.reg[0] != '\0' ? aircraft.reg
                                              : (shape[0] != '\0' ? shape : aircraft.hex));
    }
    theme::set_text(s_airframe, text);
}

void show_route(bool mine)
{
    if (mine && s_details.has_route) {
        char text[SELECTED_TEXT_LEN];
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
    lv_obj_set_hidden(s_route, lv_label_get_text(s_route)[0] == '\0');
    lv_obj_set_hidden(s_cities, lv_label_get_text(s_cities)[0] == '\0');
}

const char *climb_trend(int vertical_fpm)
{
    return vertical_fpm > LEVEL_FLIGHT_FPM    ? " " LV_SYMBOL_UP
           : vertical_fpm < -LEVEL_FLIGHT_FPM ? " " LV_SYMBOL_DOWN
                                              : "";
}

void show_selected_readings(const radar::Aircraft &aircraft)
{
    char text[SELECTED_TEXT_LEN];
    if (aircraft.altitude_ft >= 0) {
        std::snprintf(text, sizeof(text), "%d%s", aircraft.altitude_ft,
                      climb_trend(aircraft.vertical_fpm));
    } else {
        std::snprintf(text, sizeof(text), "%s", NO_READING);
    }
    set_row(ALTITUDE_READING, "feet", text, altitude_ink(aircraft.altitude_ft));

    std::snprintf(text, sizeof(text), "%.0f", static_cast<double>(aircraft.speed_kt));
    set_row(SPEED_READING, "knots", text);

    std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(distance_km(aircraft)));
    set_row(DISTANCE_READING, "km away", text);
}

void show_selected(const radar::Aircraft &aircraft)
{
    lv_obj_set_hidden(s_nearby, true);
    lv_obj_set_hidden(s_airframe, false);
    theme::set_text(s_title, blip_name(aircraft));

    const bool mine = std::strcmp(s_details_hex, aircraft.hex) == 0;
    theme::set_text(s_operator, operator_name(mine));
    show_airframe(aircraft, mine);
    show_route(mine);
    show_selected_readings(aircraft);

    const char *shout = emergency(aircraft.squawk);
    show_picture();

    theme::set_text(s_summary, shout != nullptr ? shout : (s_last->ok ? "" : "feed unreachable"));
    theme::set_text_color(s_summary, shout != nullptr ? theme::red : theme::amber);
}

void redraw_map_if_moved(float range_km)
{
    if (s_last->home_lat == s_map_lat && s_last->home_lon == s_map_lon) {
        return;
    }
    s_map_lat = s_last->home_lat;
    s_map_lon = s_last->home_lon;
    if (map_located()) {
        draw_map(s_map_lat, s_map_lon, range_km);
    }
}

// Those in range become plots, nearest first; those beyond it wait on the rim.
void plot_traffic(float range_km)
{
    s_shown      = 0;
    int rim_used = 0;
    for (int i = 0; i < s_last->count && s_shown < DRAWN_MAX; ++i) {
        const radar::Aircraft &aircraft = s_last->list[i];
        if (aircraft.on_ground) {
            continue;
        }
        const float away_km = distance_km(aircraft);
        if (away_km > range_km) {
            if (rim_used < RIM_DOTS) {
                draw_rim(s_rim[rim_used++], aircraft);
            }
            continue;
        }
        s_plots[s_shown].aircraft = &s_last->list[i];
        s_plots[s_shown].east_km  = away_km * std::sin(aircraft.bearing_deg * DEG);
        s_plots[s_shown].north_km = away_km * std::cos(aircraft.bearing_deg * DEG);
        ++s_shown;
    }
    for (int i = rim_used; i < RIM_DOTS; ++i) {
        lv_obj_set_hidden(s_rim[i], true);
    }

    std::sort(s_plots, s_plots + s_shown, [](const Plot &a, const Plot &b) {
        return a.aircraft->distance_nm < b.aircraft->distance_nm;
    });
}

void paint_legend()
{
    for (int i = 0; i < LEGEND_STEPS; ++i) {
        lv_obj_set_style_arc_color(
            s_legend[i], lv_color_hex(altitude_ink(SCALE_TOP_FT * i / (LEGEND_STEPS - 1))),
            LV_PART_MAIN);
    }
}

int plot_of(const char *hex)
{
    for (int i = 0; i < s_shown; ++i) {
        if (std::strcmp(hex, s_plots[i].aircraft->hex) == 0) {
            return i;
        }
    }
    return -1;
}

void follow_nearest()
{
    if (s_shown == 0) {
        s_chosen[0] = '\0';
        return;
    }
    const int  held = plot_of(s_chosen);
    const bool keep = held >= 0 && s_plots[held].aircraft->distance_nm <
                                       s_plots[0].aircraft->distance_nm * FOLLOW_MARGIN;
    const int at = keep ? held : 0;
    if (!keep) {
        remember_chosen(*s_plots[0].aircraft);
    }
    if (std::strcmp(s_details_hex, s_chosen) != 0) {
        s_picture = Picture::Looking;
        ask_details(s_chosen, s_plots[at].aircraft->flight);
    }
}

bool labels_clash(const Plot &a, const Plot &b, std::int32_t line)
{
    const bool apart = a.label_x > b.label_x + b.label_w + LABEL_CLEARANCE ||
                       b.label_x > a.label_x + a.label_w + LABEL_CLEARANCE ||
                       a.y > b.y + line || b.y > a.y + line;
    return !apart;
}

void find_clear_labels(bool *named)
{
    for (int i = 0; i < s_shown; ++i) {
        named[i] = true;
    }
    const std::int32_t line = marking_font()->line_height;
    for (int i = 0; i < s_shown; ++i) {
        for (int j = i + 1; j < s_shown; ++j) {
            if (labels_clash(s_plots[i], s_plots[j], line)) {
                named[i] = false;
                named[j] = false;
            }
        }
    }
}

void ink_marker()
{
    const lv_color_t accent = lv_color_hex(theme::primary);
    if (!theme::has_local_color(s_marker, LV_STYLE_BORDER_COLOR, 0, accent)) {
        lv_obj_set_style_border_color(s_marker, accent, 0);
    }
}

void clear_traffic()
{
    if (s_air_mask != nullptr) {
        std::memset(air_alpha_plane(), 0, ground_pixels());
    }
    for (int i = 0; i < LABEL_MAX; ++i) {
        lv_obj_set_hidden(s_blips[i].label, true);
    }
}

// Returns the chosen aircraft when it is among those drawn.
const radar::Aircraft *draw_traffic(const bool *named)
{
    const radar::Aircraft *chosen   = nullptr;
    int                    labelled = 0;
    for (int i = 0; i < s_shown; ++i) {
        raster_blip(s_plots[i]);

        const bool mine =
            s_chosen[0] != '\0' && std::strcmp(s_chosen, s_plots[i].aircraft->hex) == 0;
        if (mine) {
            chosen = s_plots[i].aircraft;
            lv_obj_set_pos(s_marker, s_plots[i].x - MARKER_SIZE / 2,
                           s_plots[i].y - MARKER_SIZE / 2);
            lv_obj_set_hidden(s_marker, false);
        }
        if ((named[i] || mine) && labelled < LABEL_MAX) {
            draw_label(s_blips[labelled++], s_plots[i]);
        }
    }
    if (s_air_canvas != nullptr) {
        lv_image_set_scale(s_air_canvas, LV_SCALE_NONE);
        lv_obj_invalidate(s_air_canvas);
    }
    return chosen;
}

}  // namespace

void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    s_last  = psram_array<radar::Snapshot>(1);
    s_blips = psram_array<Blip>(LABEL_MAX);
    s_plots = psram_array<Plot>(DRAWN_MAX);
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

    s_summary = theme::make_label(bezel, "", theme::amber, marking_font());
    lv_obj_set_width(s_summary, SUMMARY_W);
    lv_obj_set_style_text_align(s_summary, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_summary, marking_font()->line_height);
    lv_obj_align(s_summary, LV_ALIGN_BOTTOM_RIGHT, -(EDGE + ZOOM_D + theme::space::s),
                 -(EDGE + (ZOOM_D - marking_font()->line_height) / 2));
    quiet(s_summary);
    paint_range_buttons();
    build_column(page, card_w + COLUMN_GAP, height);
    show_radar(*s_last);
}

// Keeps what was on show: asking again only blanked the photograph while it
// was fetched anew.
void radar_page_opened()
{
    if (s_chosen[0] == '\0') {
        s_following = true;
    }
    if (s_radar_stale) {
        refresh_radar();
    }
}

void show_radar(const radar::Snapshot &snapshot)
{
    if (s_scope == nullptr || s_last == nullptr || s_blips == nullptr) {
        return;
    }
    if (&snapshot != s_last) {
        *s_last = snapshot;
    }

    const float range_km = shown_range_km();
    redraw_map_if_moved(range_km);
    plot_traffic(range_km);
    paint_legend();

    if (s_following) {
        follow_nearest();
    }

    for (int i = 0; i < s_shown; ++i) {
        place_blip(s_plots[i], range_km);
    }
    bool named[DRAWN_MAX];
    find_clear_labels(named);

    ink_marker();
    clear_traffic();
    const radar::Aircraft *chosen = draw_traffic(named);

    if (chosen != nullptr) {
        show_selected(*chosen);
    } else {
        s_chosen[0] = '\0';
        lv_obj_set_hidden(s_marker, true);
        show_summary(*s_last);
    }
}

void refresh_radar()
{
    if (s_last == nullptr) {
        return;
    }
    if (detail::s_page != detail::RADAR_PAGE) {
        s_radar_stale = true;
        return;
    }
    s_radar_stale = false;
    radar::snapshot(*s_last);
    show_radar(*s_last);
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
    s_photo_dsc.header.stride = static_cast<std::uint32_t>(width) * RGB565_BYTES_PER_PX;
    s_photo_dsc.data_size     = static_cast<std::uint32_t>(width * height) * RGB565_BYTES_PER_PX;
    s_photo_dsc.data          = static_cast<const std::uint8_t *>(pixels);

    const std::int32_t fit_h = height * INNER_W / width;
    lv_obj_set_size(s_photo_frame, INNER_W, std::min(PHOTO_H, fit_h));
    lv_obj_set_size(s_photo, INNER_W, fit_h);
    lv_obj_set_y(s_photo, std::min<std::int32_t>(0, (PHOTO_H - fit_h) / 2));

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
