#include "radar_page.h"

#include "ui_internal.h"
#include "radar_model.h"
#include "topics.h"

#if REMOTE_ENABLED
#include "lvgl_private.h"  // for the bench, the areas waiting to be drawn
#endif

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "map_data.h"
#include "theme.h"
#include "icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include <iterator>

#ifndef REMOTE_ENABLED
#define REMOTE_ENABLED 0
#endif

namespace ui {
namespace {
// A development build logs how long the scope's heavier work takes.
// Adds how long it lived to `into`, for the bench.
struct Accrue {
    std::int64_t      &into;
    const std::int64_t began = esp_timer_get_time();
    ~Accrue() { into += esp_timer_get_time() - began; }
};

struct Timed {
    const char        *what;
    const std::int64_t began = esp_timer_get_time();
    ~Timed()
    {
        if (REMOTE_ENABLED) {
            ESP_LOGI("radar_page", "%s %d ms", what,
                     static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs));
        }
    }
};

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

constexpr int MIN_POLYGON_POINTS = 3;

// Must hold the gap plus a whole line box: at 24 the N and S labels landed
// outside the scope object and were clipped against the ring.
constexpr int RIM_BAND = 36;

constexpr int          SPOKES            = 8;
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
// A fullscreen frame takes about 175 ms while zooming, so an eased 260 ms
// showed one frame nearly at the end: longer and even, it shows three or four.
constexpr std::uint32_t ZOOM_FULL_MS       = 600;
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

constexpr std::int32_t  SPINNER_SIZE      = 104;  // round the plane waiting inside it
constexpr std::int32_t  SPINNER_STROKE    = 3;
constexpr lv_opa_t      WAITING_PLANE_OPA = LV_OPA_30;
constexpr int           SPINNER_SWEEP_DEG = 90;
constexpr std::uint32_t SPINNER_TURN_MS   = 900;

constexpr int ALTITUDE_READING = 0;
constexpr int SPEED_READING    = 1;
constexpr int DISTANCE_READING = 2;
constexpr int READINGS         = 3;

constexpr std::int32_t READING_MIN_GAP = 8;

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
// Home, where the rings are centred; on the page the middle of a disc, and
// fullscreen the middle of the space beside the column, with the map past them.
std::int32_t       s_cx                         = 0;
std::int32_t       s_cy                         = 0;
std::int32_t       s_radius                     = 0;
bool               s_to_edges                   = false;  // the map and air run past the rings

float     s_shown_range = 0.0f;
int       s_zoom_from   = 0;
lv_obj_t *s_zoom_in     = nullptr;
lv_obj_t *s_zoom_out    = nullptr;
int       s_range_step  = INITIAL_RANGE_STEP;

// The page's two cards, the scope's and the column beside it, which the
// fullscreen view takes over the whole screen and gives back.
lv_obj_t    *s_page      = nullptr;
std::int32_t s_page_w    = 0;
std::int32_t s_page_h    = 0;
lv_obj_t    *s_bezel     = nullptr;
lv_obj_t    *s_column    = nullptr;
lv_obj_t    *s_full      = nullptr;  // over the whole screen while it is up
detail::ViewId s_full_view = detail::kNoView;
lv_obj_t      *s_full_desk = nullptr;  // Stand and Sit, in the corner over the map
constexpr std::int32_t FULL_DESK_INSET = 32;  // as the chips in the map's other corners
lv_obj_t    *s_full_chip = nullptr;  // into it, and out again
lv_obj_t    *s_full_clock = nullptr;
lv_obj_t    *s_full_badge = nullptr;  // the focus timer beside the time
lv_obj_t    *s_full_mark = nullptr;
lv_obj_t    *s_screw     = nullptr;

// The scope is one opaque picture, which the PPA copies to the screen whole:
// the map, drawn for each range into s_ground, and each reading's planes over a
// copy of it in s_frame, which is what shows. A layer each for water, land and
// air was blended by hand on every frame, and a zoom stretched all three.
lv_obj_t      *s_canvas    = nullptr;
std::uint16_t *s_ground    = nullptr;
std::uint16_t *s_frame     = nullptr;
std::uint8_t  *s_land_mask = nullptr;  // borders and provinces, laid over the water once it is in
std::uint8_t  *s_air_opa   = nullptr;  // the strongest air drawn at each pixel this reading
std::int32_t   s_ground_w  = 0;

// Where the air has been drawn over the map, a tile at a time: putting the
// map back under the last reading, and drawing again what either reading drew,
// is then only there, rather than the whole picture each time, which a trail
// growing did twenty times a second.
constexpr std::int32_t    AIR_TILE         = 32;
constexpr int             AIR_AREAS_MAX    = 40;  // past this, the whole picture: LVGL keeps 64 at most
std::vector<std::uint8_t> s_air_tiles;            // drawn into since the map was last put back
std::vector<std::uint8_t> s_last_tiles;           // drawn into the time before
// What each tile held when it was last drawn, so a plane drawn again where it
// was is not drawn again on the screen; 0 for not known.
std::vector<std::uint32_t> s_tile_sums;
std::int64_t               s_sum_us = 0;  // for the bench: comparing tiles, all told
// Where the chosen aircraft's way was drawn, so a trail growing puts the map
// back and draws again only there, and the planes it passes over.
std::vector<std::uint8_t>  s_way_tiles;
std::vector<std::uint8_t>  s_maybe_changed;
bool                       s_marking_way = false;
std::int32_t              s_tiles_w       = 0;
std::int32_t              s_tiles_h       = 0;
bool                      s_air_wholesale = true;  // the map changed: put all of it back, draw all again
std::int32_t   s_ground_h  = 0;
std::int32_t   s_corner    = 0;  // fullscreen, the picture's rounded corners

// Where the map reaches along each row, worked out once for the picture;
// to below from where it misses the row.
std::int16_t *s_row_from = nullptr;
std::int16_t *s_row_to   = nullptr;

// The water's crossings, gathered edge by edge into the rows each edge spans:
// testing every edge of the sea's outline on every row was most of a map.
std::int32_t *s_crossings      = nullptr;  // MASK_MAX_EDGES a row
std::uint8_t *s_crossing_count = nullptr;

lv_point_precise_t *s_map_points = nullptr;
float               s_map_lat    = 0.0f;
float               s_map_lon    = 0.0f;

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
lv_obj_t *s_photo_plane = nullptr;  // faint inside the spinner while it turns
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
char             s_picture_hex[radar::kHexLen] = {};  // whose photo s_picture is about
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
    return static_cast<std::size_t>(s_ground_w) * s_ground_h;
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

// Kilometres east and north of home, flat, as the map is drawn.
void flat_km(float lat, float lon, float &east, float &north)
{
    east  = (lon - s_last->home_lon) * KM_PER_LON * std::cos(s_last->home_lat * DEG);
    north = (lat - s_last->home_lat) * KM_PER_LAT;
}

const char *blip_name(const radar::Aircraft &aircraft)
{
    return aircraft.flight[0] != '\0' ? aircraft.flight : aircraft.hex;
}


void quiet(lv_obj_t *obj)
{
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
}

lv_point_t on_circle(float angle, float radius)
{
    return {static_cast<std::int32_t>(s_cx + std::sin(angle) * radius),
            static_cast<std::int32_t>(s_cy - std::cos(angle) * radius)};
}

lv_obj_t *s_compass[4] = {};  // N, S, E, W

void compass(lv_obj_t *parent, const char *text, char side)
{
    lv_obj_t          *label = theme::make_label(parent, text, theme::secondary, marking_font());
    s_compass[side == 'N' ? 0 : side == 'S' ? 1 : side == 'E' ? 2 : 3] = label;
    const std::int32_t w     = theme::text_width(text, marking_font());
    const std::int32_t h     = marking_font()->line_height;
    const std::int32_t edge  = s_radius + COMPASS_GAP;

    lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
    switch (side) {
        case 'N': lv_obj_set_pos(label, s_cx - w / 2, s_cy - edge - h); break;
        case 'S': lv_obj_set_pos(label, s_cx - w / 2, s_cy + edge); break;
        case 'E': lv_obj_set_pos(label, s_cx + edge, s_cy - h / 2); break;
        default: lv_obj_set_pos(label, s_cx - edge - w, s_cy - h / 2); break;
    }
    quiet(label);
}

void place_ring_label(lv_obj_t *label, std::int32_t radius)
{
    const std::int32_t w =
        theme::text_width(lv_label_get_text(label), marking_font()) + 2 * RING_LABEL_PAD;
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

void build_rings(lv_obj_t *scope, int range_km)
{
    for (int i = 1; i <= RINGS; ++i) {

        lv_obj_t *label = theme::make_label(scope, "", theme::secondary, marking_font());
        lv_obj_set_style_text_opa(label, LV_OPA_40, 0);
        lv_obj_set_style_bg_color(label, lv_color_hex(theme::panel), 0);  // the map's own colour
        lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(label, RING_LABEL_PAD, 0);
        show_ring_distance(label, i, range_km);
        quiet(label);
        s_rings[i - 1] = label;
    }
}

int plot_near(std::int32_t x, std::int32_t y)
{
    const std::int32_t reach   = 2 * (s_radius + RIM_BAND) / TAP_REACH_DIVISOR;
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

// A photo waits only for an aircraft whose photo has not come yet: it can
// come before the details, and asking for those again must not undo it.
void await_picture()
{
    if (std::strcmp(s_picture_hex, s_chosen) != 0) {
        s_picture = Picture::Looking;
        std::snprintf(s_picture_hex, sizeof(s_picture_hex), "%s", s_chosen);
    }
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
        await_picture();
        ask_details(s_chosen, aircraft.flight);
    }
}

void choose_none()
{
    s_chosen[0] = '\0';
    forget_details();
    s_picture        = Picture::Idle;
    s_picture_hex[0] = '\0';
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
        lv_obj_set_pos(seg, s_cx - outer, s_cy - outer);
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
    const std::int32_t w     = theme::text_width(text, marking_font());
    const std::int32_t h     = marking_font()->line_height;
    const float        half  = static_cast<float>(w) / 2.0f / static_cast<float>(mid) / DEG;
    const float        angle = end_deg + direction * (half + KEY_LABEL_GAP_DEG);
    const float        rad   = angle * DEG;
    lv_obj_t *label = theme::make_label(scope, text, theme::secondary, marking_font());
    lv_obj_set_style_text_opa(label, LV_OPA_60, 0);
    lv_obj_set_pos(label, s_cx + static_cast<std::int32_t>(std::cos(rad) * mid) - w / 2,
                   s_cy + static_cast<std::int32_t>(std::sin(rad) * mid) - h / 2);
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

std::uint16_t rgb565(std::uint32_t colour)
{
    return lv_color_to_u16(lv_color_hex(colour));
}

std::uint16_t blend565(std::uint16_t under, std::uint16_t over, std::uint8_t opa)
{
    const auto channel = [&](int shift, int mask) {
        const int u = (under >> shift) & mask;
        const int o = (over >> shift) & mask;
        return ((u * (LV_OPA_COVER - opa) + o * opa) / LV_OPA_COVER) << shift;
    };
    constexpr int RED = 11, GREEN = 5, FIVE_BITS = 0x1f, SIX_BITS = 0x3f;
    return static_cast<std::uint16_t>(channel(RED, FIVE_BITS) | channel(GREEN, SIX_BITS) | channel(0, FIVE_BITS));
}

// Along row y of a circle `radius` round cx, cy; false where it misses the row.
bool circle_span(std::int32_t y, std::int32_t cx, std::int32_t cy, float radius, std::int32_t &from,
                 std::int32_t &to)
{
    const float dy   = static_cast<float>(y - cy) + 0.5f;
    const float span = radius * radius - dy * dy;
    if (span < 0.0f) {
        return false;
    }
    const auto half = static_cast<std::int32_t>(std::sqrt(span));
    from            = std::max<std::int32_t>(cx - half, 0);
    to              = std::min<std::int32_t>(cx + half, s_ground_w - 1);
    return from <= to;
}

// Along row y of the whole picture less its rounded corners.
void rounded_span(std::int32_t y, std::int32_t &from, std::int32_t &to)
{
    from = 0;
    to   = s_ground_w - 1;
    const std::int32_t r  = s_corner;
    const float        dy = y < r ? static_cast<float>(r - y) - 0.5f
                          : y >= s_ground_h - r ? static_cast<float>(y - (s_ground_h - r)) + 0.5f
                                                : 0.0f;
    if (dy > 0.0f) {
        const auto inset = static_cast<std::int32_t>(
            std::ceil(static_cast<float>(r) - std::sqrt(std::max(0.0f, static_cast<float>(r * r) - dy * dy))));
        from = inset;
        to   = s_ground_w - 1 - inset;
    }
}

// Where the map reaches along row y: inside the last ring on the page, and
// fullscreen the whole picture but its corners.
bool reach_of(std::int32_t y, std::int32_t &from, std::int32_t &to)
{
    if (!s_to_edges) {
        return circle_span(y, s_cx, s_cy, static_cast<float>(s_radius), from, to);
    }
    rounded_span(y, from, to);
    return from <= to;
}

void work_out_rows()
{
    for (std::int32_t y = 0; y < s_ground_h; ++y) {
        std::int32_t from = 0, to = -1;
        if (!reach_of(y, from, to)) {
            from = 0;
            to   = -1;
        }
        s_row_from[y] = static_cast<std::int16_t>(from);
        s_row_to[y]   = static_cast<std::int16_t>(to);
    }
}

bool map_span(std::int32_t y, std::int32_t &from, std::int32_t &to)
{
    if (y < 0 || y >= s_ground_h || s_row_from == nullptr) {
        return false;
    }
    from = s_row_from[y];
    to   = s_row_to[y];
    return from <= to;
}

// Where planes may be drawn: anywhere on the page's square, which only plots
// those inside the last ring, and fullscreen where the map is.
bool air_span(std::int32_t y, std::int32_t &from, std::int32_t &to)
{
    if (s_to_edges) {
        return map_span(y, from, to);
    }
    from = 0;
    to   = s_ground_w - 1;
    return y >= 0 && y < s_ground_h;
}

bool within_scope(std::int32_t x, std::int32_t y)
{
    std::int32_t from = 0, to = 0;
    return map_span(y, from, to) && x >= from && x <= to;
}

// Each pixel of a line that is on the map, handed to `put`. A line wholly off
// the picture to one side is passed over: the map reaches far past it.
// Cut to the picture, Liang and Barsky's way; false when none of it is on it.
bool clip_to_picture(std::int32_t &x0, std::int32_t &y0, std::int32_t &x1, std::int32_t &y1)
{
    const auto  dx   = static_cast<float>(x1 - x0);
    const auto  dy   = static_cast<float>(y1 - y0);
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {static_cast<float>(x0), static_cast<float>(s_ground_w - 1 - x0),
                        static_cast<float>(y0), static_cast<float>(s_ground_h - 1 - y0)};
    float       t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) {
                return false;
            }
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0.0f) {
            t0 = std::max(t0, t);
        } else {
            t1 = std::min(t1, t);
        }
        if (t0 > t1) {
            return false;
        }
    }
    const auto ox = static_cast<float>(x0), oy = static_cast<float>(y0);
    x0 = static_cast<std::int32_t>(std::lround(ox + t0 * dx));
    y0 = static_cast<std::int32_t>(std::lround(oy + t0 * dy));
    x1 = static_cast<std::int32_t>(std::lround(ox + t1 * dx));
    y1 = static_cast<std::int32_t>(std::lround(oy + t1 * dy));
    return true;
}

template <typename Put>
void walk_line(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, const Put &put)
{
    if (!clip_to_picture(x0, y0, x1, y1)) {
        return;
    }
    const std::int32_t dx     = std::abs(x1 - x0);
    const std::int32_t dy     = -std::abs(y1 - y0);
    const std::int32_t step_x = x0 < x1 ? 1 : -1;
    const std::int32_t step_y = y0 < y1 ? 1 : -1;
    std::int32_t       error  = dx + dy;

    for (;;) {
        if (within_scope(x0, y0)) {
            put(static_cast<std::size_t>(y0) * s_ground_w + x0);
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

void fill_water_row(std::int32_t y, const std::int32_t *crossings, int found, std::uint16_t water)
{
    std::int32_t low = 0, high = 0;
    if (!map_span(y, low, high)) {
        return;
    }

    for (int i = 0; i + 1 < found; i += 2) {
        const std::int32_t from = std::max(crossings[i], low);
        const std::int32_t to   = std::min(crossings[i + 1], high);
        if (to < from) {
            continue;
        }
        std::uint16_t *row = s_ground + static_cast<std::size_t>(y) * s_ground_w;
        std::fill(row + from, row + to + 1, water);
    }
}

void fill_water(const lv_point_precise_t *points, const std::uint16_t *rings, int ring_count,
                int count, std::uint16_t water)
{
    if (count < MIN_POLYGON_POINTS || s_crossings == nullptr) {
        return;
    }
    const Rows         rows   = rows_spanned(points, count);
    const std::int32_t top    = std::max<std::int32_t>(rows.top, 0);
    const std::int32_t bottom = std::min<std::int32_t>(rows.bottom, s_ground_h - 1);
    if (top > bottom) {
        return;
    }
    std::memset(s_crossing_count + top, 0, static_cast<std::size_t>(bottom - top + 1));

    // An edge crosses row y when y lies in [its lower end, its upper end), as
    // add_crossings counts it.
    int base = 0;
    for (int r = 0; r < ring_count; ++r) {
        const int length = rings[r];
        for (int i = 0, j = length - 1; i < length && base + i < count; j = i++) {
            const lv_point_precise_t &a = points[base + i];
            const lv_point_precise_t &b = points[base + j];
            if (a.y == b.y) {
                continue;
            }
            const std::int32_t lo    = std::max<std::int32_t>(std::min(a.y, b.y), top);
            const std::int32_t hi    = std::min<std::int32_t>(std::max(a.y, b.y) - 1, bottom);
            const float        slope = static_cast<float>(b.x - a.x) / static_cast<float>(b.y - a.y);
            for (std::int32_t y = lo; y <= hi; ++y) {
                std::uint8_t &found = s_crossing_count[y];
                if (found < MASK_MAX_EDGES) {
                    s_crossings[static_cast<std::size_t>(y) * MASK_MAX_EDGES + found++] =
                        static_cast<std::int32_t>(std::lround(static_cast<float>(a.x) +
                                                              static_cast<float>(y - a.y) * slope));
                }
            }
        }
        base += length;
    }
    for (std::int32_t y = top; y <= bottom; ++y) {
        const int found = s_crossing_count[y];
        if (found < 2) {
            continue;
        }
        std::int32_t *row = s_crossings + static_cast<std::size_t>(y) * MASK_MAX_EDGES;
        std::sort(row, row + found);
        fill_water_row(y, row, found, water);
    }
}

struct Projection {
    float home_lat;
    float home_lon;
    float px_per_lat;
    float px_per_lon;
};

Projection projection_for(float home_lat, float home_lon, float range_km)
{
    const float scale = static_cast<float>(s_radius) / range_km;
    return {home_lat, home_lon, KM_PER_LAT * scale, KM_PER_LON * std::cos(home_lat * DEG) * scale};
}

lv_point_precise_t project(const std::int32_t *lat_lon, const Projection &projection)
{
    const auto  cx    = static_cast<float>(s_cx);
    const auto  cy    = static_cast<float>(s_cy);
    const float reach = static_cast<float>(std::max({s_radius, s_ground_w, s_ground_h})) * MAP_REACH_RADII;
    const float lat   = static_cast<float>(lat_lon[0]) / MICRODEGREES_PER_DEG;
    const float lon   = static_cast<float>(lat_lon[1]) / MICRODEGREES_PER_DEG;
    const float x     = cx + (lon - projection.home_lon) * projection.px_per_lon;
    const float y     = cy - (lat - projection.home_lat) * projection.px_per_lat;

    const float clamped_x = std::clamp(x, cx - reach, cx + reach);
    const float clamped_y = std::clamp(y, cy - reach, cy + reach);
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

// A river is water, drawn at once; borders go onto the land mask, laid over the
// water when all of it is in.
void stroke_path(radar::MapLayer layer, int count, std::uint16_t river)
{
    const bool         water = layer == radar::MapLayer::River;
    const std::uint8_t value = layer == radar::MapLayer::Province ? PROVINCE_ALPHA : LINE_ALPHA;
    for (int v = 1; v < count; ++v) {
        walk_line(s_map_points[v - 1].x, s_map_points[v - 1].y, s_map_points[v].x, s_map_points[v].y,
                  [&](std::size_t at) {
                      if (water) {
                          s_ground[at] = river;
                      } else {
                          s_land_mask[at] = value;
                      }
                  });
    }
}

bool is_water_body(radar::MapLayer layer)
{
    return layer == radar::MapLayer::Ocean || layer == radar::MapLayer::Lake;
}

// What the map lies on, the cards' darker surface: on the page the scope's
// disc in the card, and fullscreen a panel rounded as the cards are.
std::uint16_t land_colour()
{
    return rgb565(theme::panel);
}

void paint_base()
{
    const std::uint16_t land   = land_colour();
    const std::uint16_t around = rgb565(s_to_edges ? theme::background : theme::panel_light);
    const std::uint16_t disc   = land;
    // Each pixel written once: the whole picture twice over took 30 ms.
    for (std::int32_t y = 0; y < s_ground_h; ++y) {
        std::uint16_t *row  = s_ground + static_cast<std::size_t>(y) * s_ground_w;
        std::int32_t   from = 0, to = 0;
        if (s_to_edges) {
            rounded_span(y, from, to);
        } else if (!circle_span(y, s_cx, s_cy, static_cast<float>(s_ground_w) / 2.0f, from, to)) {
            std::fill(row, row + s_ground_w, around);
            continue;
        }
        std::fill(row, row + from, around);
        std::fill(row + from, row + to + 1, disc);
        std::fill(row + to + 1, row + s_ground_w, around);
    }
}

// The range rings and the spokes, drawn into the picture over the map. As
// LVGL objects each ring tested every pixel of its square on every frame,
// most of a frame's time; here only the pixels near a ring are touched, and
// which and how strongly is worked out once a scope, as every map redrawn
// during a zoom took 11 ms of it again.
std::uint32_t *s_ring_at    = nullptr;
std::uint8_t  *s_ring_opa   = nullptr;
std::uint16_t *s_ring_under = nullptr;  // what each ring pixel was before the ring, to take the rings out again
std::size_t    s_ring_count = 0;

template <typename Put>
void trace_ring(float radius, lv_opa_t opa, Put &put)
{
    const float  outer = radius;
    const float  inner = radius - static_cast<float>(RING_STROKE);
    const auto   cx    = static_cast<float>(s_cx);
    const auto   cy    = static_cast<float>(s_cy);
    const auto   top   = std::max<std::int32_t>(0, static_cast<std::int32_t>(cy - outer) - 1);
    const auto   foot  = std::min<std::int32_t>(s_ground_h - 1, static_cast<std::int32_t>(cy + outer) + 1);
    const auto   shade = [&](std::int32_t x, std::int32_t y, float dy) {
        if (x < 0 || x >= s_ground_w) {
            return;
        }
        const float dx       = static_cast<float>(x) + 0.5f - cx;
        const float d        = std::sqrt(dx * dx + dy * dy);
        const float coverage = std::clamp(std::min(d - inner, outer - d) + 0.5f, 0.0f, 1.0f);
        if (coverage > 0.0f) {
            put(static_cast<std::size_t>(y) * s_ground_w + x,
                static_cast<std::uint8_t>(static_cast<float>(opa) * coverage));
        }
    };
    for (std::int32_t y = top; y <= foot; ++y) {
        const float dy   = static_cast<float>(y) + 0.5f - cy;
        const float far  = (outer + 1.0f) * (outer + 1.0f) - dy * dy;
        if (far < 0.0f) {
            continue;
        }
        const float near  = (inner - 1.0f) * (inner - 1.0f) - dy * dy;
        const auto  reach = static_cast<std::int32_t>(std::ceil(std::sqrt(far)));
        const auto  hole  = near > 0.0f ? static_cast<std::int32_t>(std::floor(std::sqrt(near))) : -1;
        for (std::int32_t x = std::max<std::int32_t>(s_cx - reach, 0); x <= std::min(s_cx + reach, s_ground_w - 1); ++x) {
            if (hole >= 0 && x > s_cx - hole && x < s_cx + hole) {
                x = s_cx + hole;  // across the inside, to the far side
            }
            shade(x, y, dy);
        }
    }
}

template <typename Put>
void trace_rings_and_spokes(Put &put)
{
    for (int i = 1; i <= RINGS; ++i) {
        trace_ring(static_cast<float>(s_radius * i / RINGS), RING_OPA[i - 1], put);
    }
    const auto inner = static_cast<float>(s_radius) / RINGS;
    const auto outer = static_cast<float>(s_radius);
    for (int i = 0; i < SPOKES; ++i) {
        const float      angle = static_cast<float>(i) * FULL_TURN_DEG / SPOKES * DEG;
        const lv_point_t from  = on_circle(angle, inner);
        const lv_point_t to    = on_circle(angle, outer);
        walk_line(from.x, from.y, to.x, to.y, [&](std::size_t at) { put(at, LV_OPA_10); });
    }
}

void work_out_rings()
{
    heap_caps_free(s_ring_at);
    heap_caps_free(s_ring_opa);
    heap_caps_free(s_ring_under);
    s_ring_at    = nullptr;
    s_ring_opa   = nullptr;
    s_ring_under = nullptr;
    s_ring_count = 0;
    std::size_t count = 0;
    auto        tally = [&count](std::size_t, std::uint8_t) { ++count; };
    trace_rings_and_spokes(tally);
    s_ring_at  = static_cast<std::uint32_t *>(heap_caps_malloc(count * sizeof(std::uint32_t), MALLOC_CAP_SPIRAM));
    s_ring_opa = static_cast<std::uint8_t *>(heap_caps_malloc(count, MALLOC_CAP_SPIRAM));
    s_ring_under = static_cast<std::uint16_t *>(heap_caps_malloc(count * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM));
    if (s_ring_at == nullptr || s_ring_opa == nullptr || s_ring_under == nullptr) {
        return;
    }
    auto keep = [](std::size_t at, std::uint8_t opa) {
        s_ring_at[s_ring_count]  = static_cast<std::uint32_t>(at);
        s_ring_opa[s_ring_count] = opa;
        ++s_ring_count;
    };
    trace_rings_and_spokes(keep);
}

void draw_rings_and_spokes()
{
    const std::uint16_t ink = rgb565(theme::secondary);
    for (std::size_t i = 0; i < s_ring_count; ++i) {
        std::uint16_t &pixel = s_ground[s_ring_at[i]];
        s_ring_under[i]      = pixel;
        pixel                = blend565(pixel, ink, s_ring_opa[i]);
    }
}

// The borders' mask laid over the map, and cleared behind it for the next
// map. Most of it is empty, so it is read four pixels at a time.
void lay_mask(std::uint16_t ink)
{
    const std::size_t n     = ground_pixels();
    const std::size_t words = n / sizeof(std::uint32_t);
    auto             *mask  = reinterpret_cast<std::uint32_t *>(s_land_mask);
    const auto        lay   = [ink](std::size_t i) {
        if (s_land_mask[i] != 0) {
            s_ground[i]    = blend565(s_ground[i], ink, s_land_mask[i]);
            s_land_mask[i] = 0;
        }
    };
    for (std::size_t w = 0; w < words; ++w) {
        if (mask[w] != 0) {
            for (std::size_t i = w * sizeof(std::uint32_t); i < (w + 1) * sizeof(std::uint32_t); ++i) {
                lay(i);
            }
        }
    }
    for (std::size_t i = words * sizeof(std::uint32_t); i < n; ++i) {
        lay(i);
    }
}

void draw_map(float home_lat, float home_lon, float range_km)
{
    const Timed timed{"map"};
    if (s_ground == nullptr || s_land_mask == nullptr || s_map_points == nullptr) {
        return;
    }
    std::int64_t mark = esp_timer_get_time();
    const auto   lap  = [&mark] {
        const std::int64_t now = esp_timer_get_time();
        const int          ms  = static_cast<int>((now - mark) / units::kUsPerMs);
        mark                   = now;
        return ms;
    };
    paint_base();
    s_air_wholesale = true;
    const int painted = lap();

    const std::uint16_t land  = land_colour();
    const std::uint16_t water = blend565(land, rgb565(INK_WATER), WATER_ALPHA);
    const std::uint16_t river = blend565(land, rgb565(INK_WATER), LINE_ALPHA);
    const Projection    projection = projection_for(home_lat, home_lon, range_km);
    for (int i = 0; i < radar::kMapPathCount; ++i) {
        const radar::MapPath &path  = radar::kMapPaths[i];
        const int             count = project_path(path, projection);
        if (is_water_body(path.layer)) {
            fill_water(s_map_points, path.rings, path.ring_count, count, water);
        } else {
            stroke_path(path.layer, count, river);
        }
    }

    const int drawn = lap();
    const std::uint16_t ink = rgb565(theme::secondary);
    lay_mask(ink);
    const int laid = lap();
    draw_rings_and_spokes();
    if (REMOTE_ENABLED) {
        ESP_LOGI("radar_page", "map: painted %d, paths %d, borders laid %d, rings %d", painted, drawn, laid, lap());
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

void clear_traffic();
void hide_labels(int first);
void restore_tiles(const std::vector<std::uint8_t> &tiles);
void plot_traffic(float range_km);
void place_blip(Plot &plot, float range_km);
void raster_blip(const Plot &plot);

// Each step of a zoom draws the map again at the range in between, and the
// planes on it, without their names: stretching the last picture blurred it,
// spilled it past the scope's edge, and was slower to show.
bool s_ground_spent = false;  // a zoom drew its planes onto the map itself, which is to be drawn again
bool s_fast_zooming = false;  // the PPA is magnifying the picture, which is to stay as it is meanwhile

// The map at the range in between is drawn into the buffer behind the
// picture and shown as it is, with the planes drawn onto it: copying it over
// the picture first, and clearing the air's opacities, was a third of a step.
void draw_step(float range_km)
{
    const Timed timed{"zoom step"};
    if (map_located() && s_frame != nullptr && s_canvas != nullptr) {
        draw_map(s_map_lat, s_map_lon, range_km);
        std::swap(s_ground, s_frame);
        lv_canvas_set_buffer(s_canvas, s_frame, s_ground_w, s_ground_h, LV_COLOR_FORMAT_RGB565);
        s_ground_spent = true;
    } else {
        clear_traffic();
    }
    hide_labels(0);
    plot_traffic(range_km);
    for (int i = 0; i < s_shown; ++i) {
        place_blip(s_plots[i], range_km);
        raster_blip(s_plots[i]);
    }
    lv_obj_set_hidden(s_marker, true);
    if (s_canvas != nullptr) {
        lv_obj_invalidate(s_canvas);
    }
}

// For the bench: how far along each frame of a zoom was drawn.
constexpr int ZOOM_TRACE_MAX = 16;
std::int32_t  s_zoom_trace[ZOOM_TRACE_MAX];
int           s_zoom_traced = -1;  // not tracing

void zoom_step(void *, std::int32_t value)
{
    if (value == 0) {
        return;  // as it was already: a fullscreen frame of nothing moving
    }
    if (REMOTE_ENABLED && s_zoom_traced >= 0 && s_zoom_traced < ZOOM_TRACE_MAX) {
        s_zoom_trace[s_zoom_traced++] = value;
    }
    const auto  from = static_cast<float>(s_zoom_from);
    const auto  to   = static_cast<float>(RANGES[s_range_step]);
    const float t    = static_cast<float>(value) / static_cast<float>(ZOOM_PROGRESS_FULL);
    s_shown_range    = from + (to - from) * t;
    draw_step(s_shown_range);
}

void settle_zoom(bool map_drawn = false)
{
    const int settled = RANGES[s_range_step];
    s_shown_range     = static_cast<float>(settled);
    if (map_located() && (!map_drawn || s_ground_spent)) {
        draw_map(s_map_lat, s_map_lon, static_cast<float>(settled));
    }
    s_ground_spent = false;
    show_radar(*s_last);
}

// The animation's last step drew the map at the range it ends on.
void zoom_done(lv_anim_t *)
{
    settle_zoom(s_shown_range == static_cast<float>(RANGES[s_range_step]));
}

#ifdef ESP_PLATFORM
// Fullscreen, a zoom is the picture magnified by the PPA straight onto the
// panel, frame by frame, with LVGL drawing nothing meanwhile: each frame of it
// drawn by LVGL was a whole screen, 88 ms, as well as the map drawn again. The
// picture magnified is the wider of the two ranges, so it only ever grows and
// is cropped: going out, the new one is drawn first and shown as it would be
// at the old range; going in, the old one grows to the new. Its end is drawn
// by LVGL again, sharp, with the names.
// One frame for each magnification the PPA has, a sixteenth apart, as fast as
// they come, so each frame moves as far as the last; past this many, every
// other one.
constexpr int FAST_ZOOM_FRAMES_MAX = 10;
lv_timer_t   *s_fast_timer  = nullptr;
std::int32_t  s_fast_step   = 0;   // the magnification shown next, in sixteenths
std::int32_t  s_fast_end    = 0;   // and the last
std::int32_t  s_fast_stride = 1;
int  s_fast_from    = 0;    // the ranges it goes between
int  s_fast_to      = 0;
int  s_fast_picture = 0;    // the range of the picture magnified
int  s_fast_frames  = 0;    // for the bench
std::int32_t s_fast_shown = -1;  // the magnification last shown, in sixteenths

// The rings and spokes out of the picture, where no air was drawn over them:
// a zoom draws them where they stay, over the map it magnifies.
void take_rings_out()
{
    if (s_ring_under == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < s_ring_count; ++i) {
        if (s_air_opa[s_ring_at[i]] == 0) {
            s_frame[s_ring_at[i]] = s_ring_under[i];
        }
    }
}

void show_magnified(std::int32_t steps)
{
    s_fast_shown = steps;
    lv_area_t picture;
    lv_obj_get_coords(s_canvas, &picture);
    lv_area_t to = picture;
    lv_area_t column;
    lv_obj_get_coords(s_column, &column);
    to.x2 = std::min<std::int32_t>(to.x2, column.x1 - 1);  // the column stands as it is
    board::ZoomFrame frame;
    frame.picture = s_frame;
    frame.w       = s_ground_w;
    frame.h       = s_ground_h;
    frame.x       = picture.x1;
    frame.y       = picture.y1;
    frame.cx      = static_cast<float>(s_cx);
    frame.cy      = static_cast<float>(s_cy);
    frame.scale   = static_cast<float>(steps) / 16.0f;
    frame.to      = to;
    frame.ring_at    = s_ring_under != nullptr ? s_ring_at : nullptr;
    frame.ring_opa   = s_ring_opa;
    frame.ring_count = s_ring_count;
    frame.ring_ink   = rgb565(theme::secondary);
    board::zoom_frame(frame);
    ++s_fast_frames;
}

void fast_zoom_done();

void fast_zoom_tick(lv_timer_t *)
{
    show_magnified(s_fast_step);
    const bool last = s_fast_step == s_fast_end;
    const std::int32_t way = s_fast_end > s_fast_step ? s_fast_stride : -s_fast_stride;
    s_fast_step = (way > 0 ? std::min(s_fast_step + way, s_fast_end) : std::max(s_fast_step + way, s_fast_end));
    if (last) {
        lv_timer_delete(s_fast_timer);
        s_fast_timer = nullptr;
        fast_zoom_done();
    }
}

void fast_zoom_done()
{
    const bool in = s_fast_to < s_fast_from;
    if (in) {
        draw_map(s_map_lat, s_map_lon, static_cast<float>(s_fast_to));
    }
    board::zoom_end();
    s_fast_zooming = false;
    s_shown_range  = static_cast<float>(s_fast_to);
    lv_display_t *disp = lv_display_get_default();
    lv_display_enable_invalidation(disp, true);
    show_radar(*s_last);
    lv_obj_invalidate(lv_screen_active());
}

bool start_fast_zoom(int from_km)
{
    if (s_fast_zooming) {
        // A tap during one: that one ends where it was going, and this goes on from there.
        if (s_fast_timer != nullptr) {
            lv_timer_delete(s_fast_timer);
            s_fast_timer = nullptr;
        }
        fast_zoom_done();
        from_km = s_fast_to;
    }
    const int to_km = RANGES[s_range_step];
    if (!s_to_edges || !map_located() || s_canvas == nullptr || s_frame == nullptr || from_km == to_km) {
        return false;
    }
    lv_anim_delete(s_scope, nullptr);
    lv_refr_now(nullptr);  // what waits to be drawn, before LVGL is held off
    // The controls over the picture, the rings' distances, already the new
    // ones, and the compass's letters, are kept as they are now.
    lv_area_t keep[16];
    int       kept = 0;
    for (lv_obj_t *control : {s_zoom_out, s_zoom_in, s_full_chip, s_full_desk, s_full_clock, s_full_badge, s_rings[0],
                              s_rings[1], s_rings[2], s_rings[3], s_compass[0], s_compass[1], s_compass[2], s_compass[3]}) {
        if (control != nullptr && !lv_obj_has_flag(control, LV_OBJ_FLAG_HIDDEN) && kept < 16) {
            lv_obj_get_coords(control, &keep[kept++]);
        }
    }
    board::zoom_begin(keep, kept);
    lv_display_enable_invalidation(lv_display_get_default(), false);
    s_fast_zooming = true;

    s_fast_from   = from_km;
    s_fast_to     = to_km;
    s_fast_shown  = -1;
    s_fast_frames = 0;
    if (to_km > from_km) {
        // Out: the wider picture first, planes and all, unseen till the zoom shows it.
        draw_map(s_map_lat, s_map_lon, static_cast<float>(to_km));
        clear_traffic();
        hide_labels(0);
        plot_traffic(static_cast<float>(to_km));
        for (int i = 0; i < s_shown; ++i) {
            place_blip(s_plots[i], static_cast<float>(to_km));
            raster_blip(s_plots[i]);
        }
    }
    s_fast_picture = std::max(from_km, to_km);
    lv_obj_set_hidden(s_marker, true);
    take_rings_out();

    // From the magnification showing the old range to the one showing the new,
    // the first after the one on show, the last as near the new as there is.
    const auto sixteenths = [](float scale) { return static_cast<std::int32_t>(std::lround(scale * 16.0f)); };
    const std::int32_t from = sixteenths(static_cast<float>(s_fast_picture) / static_cast<float>(from_km));
    s_fast_end              = sixteenths(static_cast<float>(s_fast_picture) / static_cast<float>(to_km));
    const std::int32_t span = std::abs(s_fast_end - from);
    s_fast_stride           = std::max<std::int32_t>(1, (span + FAST_ZOOM_FRAMES_MAX - 1) / FAST_ZOOM_FRAMES_MAX);
    s_fast_step             = to_km > from_km ? from : from + (s_fast_end > from ? s_fast_stride : -s_fast_stride);
    if (s_fast_timer != nullptr) {
        lv_timer_delete(s_fast_timer);
    }
    s_fast_timer = lv_timer_create(fast_zoom_tick, 1, nullptr);
    return true;
}
#endif

void start_zoom(int from_km)
{
#ifdef ESP_PLATFORM
    if (start_fast_zoom(from_km)) {
        return;
    }
#endif
    s_zoom_from = from_km;
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_scope);
    lv_anim_set_values(&anim, 0, ZOOM_PROGRESS_FULL);
    lv_anim_set_duration(&anim, s_to_edges ? ZOOM_FULL_MS : ZOOM_MS);
    lv_anim_set_exec_cb(&anim, zoom_step);
    lv_anim_set_completed_cb(&anim, zoom_done);
    lv_anim_set_path_cb(&anim, s_to_edges ? lv_anim_path_linear : lv_anim_path_ease_out);
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

// Round chips in the card's corners, the same as the heating card's: a dot of
// the darker surface on the lighter card is what makes it read as something to
// press. The hit area reaches past the dot, so a finger does not have to find it.
lv_obj_t *zoom_chip(lv_obj_t *bezel, const lv_image_dsc_t *mark, int step)
{
    lv_obj_t *chip = theme::make_chip(bezel, "");
    theme::make_mark(chip, mark);
    lv_obj_set_ext_click_area(chip, (CORNER - ZOOM_D) / 2);
    lv_obj_add_event_cb(chip, range_clicked, LV_EVENT_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(step)));
    return chip;
}

void full_clicked(lv_event_t *);

// Zoom along the bottom, where the thumbs rest; fullscreen at the top right,
// as the focus dial and the cinema card have it; a screw in the last corner.
void build_corners(lv_obj_t *bezel)
{
    s_zoom_out  = zoom_chip(bezel, &icons::minus_icon, 1);
    s_zoom_in   = zoom_chip(bezel, &icons::plus_icon, -1);
    s_screw     = theme::make_screw(bezel, ZOOM_D);
    s_full_chip = theme::make_chip(bezel, "");
    s_full_mark = theme::make_mark(s_full_chip, &icons::expand_icon);
    lv_obj_set_ext_click_area(s_full_chip, (CORNER - ZOOM_D) / 2);
    lv_obj_add_event_cb(s_full_chip, full_clicked, LV_EVENT_CLICKED, nullptr);
}

// On the page the chips are the darker surface on the lighter card; fullscreen
// the map is that surface, so they take the black of the margin.
void paint_chips(bool full)
{
    for (lv_obj_t *chip : {s_zoom_out, s_zoom_in, s_full_chip}) {
        lv_obj_set_style_bg_color(chip, lv_color_hex(full ? theme::background : theme::panel), 0);
    }
}

void place_corners(std::int32_t w, std::int32_t h)
{
    lv_obj_set_pos(s_screw, EDGE, EDGE);
    lv_obj_set_pos(s_full_chip, w - ZOOM_D - EDGE, EDGE);
    lv_obj_set_pos(s_zoom_out, EDGE, h - ZOOM_D - EDGE);
    lv_obj_set_pos(s_zoom_in, w - ZOOM_D - EDGE, h - ZOOM_D - EDGE);
}

// Aligned for the PPA, which reads the picture straight from memory.
constexpr std::size_t PICTURE_ALIGN = 128;

void build_picture(lv_obj_t *scope, std::int32_t w, std::int32_t h)
{
    s_ground_w = w;
    s_ground_h = h;
    const std::size_t n = ground_pixels();
    s_ground    = static_cast<std::uint16_t *>(
        heap_caps_aligned_alloc(PICTURE_ALIGN, n * RGB565_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    s_frame     = static_cast<std::uint16_t *>(
        heap_caps_aligned_alloc(PICTURE_ALIGN, n * RGB565_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    s_land_mask = static_cast<std::uint8_t *>(heap_caps_calloc(n, ALPHA_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    s_air_opa   = static_cast<std::uint8_t *>(heap_caps_calloc(n, ALPHA_BYTES_PER_PX, MALLOC_CAP_SPIRAM));
    s_row_from  = static_cast<std::int16_t *>(heap_caps_malloc(h * sizeof(std::int16_t), MALLOC_CAP_SPIRAM));
    s_row_to    = static_cast<std::int16_t *>(heap_caps_malloc(h * sizeof(std::int16_t), MALLOC_CAP_SPIRAM));
    s_crossings = static_cast<std::int32_t *>(
        heap_caps_malloc(static_cast<std::size_t>(h) * MASK_MAX_EDGES * sizeof(std::int32_t), MALLOC_CAP_SPIRAM));
    s_crossing_count = static_cast<std::uint8_t *>(heap_caps_malloc(h, MALLOC_CAP_SPIRAM));
    s_map_points = static_cast<lv_point_precise_t *>(heap_caps_malloc(
        sizeof(lv_point_precise_t) * MAP_POINTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_ground == nullptr || s_frame == nullptr || s_land_mask == nullptr || s_air_opa == nullptr ||
        s_row_from == nullptr || s_row_to == nullptr || s_crossings == nullptr || s_crossing_count == nullptr) {
        return;
    }
    s_tiles_w = (w + AIR_TILE - 1) / AIR_TILE;
    s_tiles_h = (h + AIR_TILE - 1) / AIR_TILE;
    s_air_tiles.assign(static_cast<std::size_t>(s_tiles_w) * s_tiles_h, 0);
    s_last_tiles.assign(s_air_tiles.size(), 0);
    s_tile_sums.assign(s_air_tiles.size(), 0);
    s_ground_spent = false;
    s_way_tiles.assign(s_air_tiles.size(), 0);
    s_maybe_changed.assign(s_air_tiles.size(), 0);
    s_air_wholesale = true;
    work_out_rows();
    work_out_rings();
    paint_base();
    draw_rings_and_spokes();  // there before any map is
    std::memcpy(s_frame, s_ground, n * RGB565_BYTES_PER_PX);
    s_canvas = lv_canvas_create(scope);
    lv_canvas_set_buffer(s_canvas, s_frame, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    quiet(s_canvas);
}

void build_home(lv_obj_t *scope)
{
    lv_obj_t *home = lv_obj_create(scope);
    lv_obj_set_size(home, HOME_SIZE, HOME_SIZE);
    lv_obj_set_pos(home, s_cx - HOME_SIZE / 2, s_cy - HOME_SIZE / 2);
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

void build_blip_labels(lv_obj_t *scope)
{
    for (int i = 0; i < LABEL_MAX; ++i) {
        s_blips[i].label = theme::make_label(scope, "", theme::secondary, marking_font());
        lv_obj_set_hidden(s_blips[i].label, true);
        quiet(s_blips[i].label);
    }
}

// A disc `side` across on the page; fullscreen `w` by `h`, the rings round
// cx, cy and the map and air past them to every edge.
struct Frame {
    std::int32_t w, h, cx, cy, radius;
    bool         to_edges;
    std::int32_t corner;  // of the picture, fullscreen
};

void build_scope(lv_obj_t *parent, const Frame &frame, int range_km)
{
    s_cx       = frame.cx;
    s_cy       = frame.cy;
    s_radius   = frame.radius;
    s_to_edges = frame.to_edges;
    s_corner   = frame.corner;
    const std::int32_t side = frame.w;

    lv_obj_t *scope = lv_obj_create(parent);
    lv_obj_set_pos(scope, 0, 0);
    lv_obj_set_size(scope, frame.w, frame.h);
    theme::style_panel(scope, theme::background, frame.to_edges ? 0 : side / 2);
    lv_obj_set_style_bg_opa(scope, LV_OPA_TRANSP, 0);  // the picture covers it, corners and all
    lv_obj_set_scrollable(scope, false);
    lv_obj_add_event_cb(scope, scope_clicked, LV_EVENT_CLICKED, nullptr);

    build_picture(scope, frame.w, frame.h);
    build_rings(scope, range_km);
    compass(scope, "N", 'N');
    compass(scope, "S", 'S');
    compass(scope, "E", 'E');
    compass(scope, "W", 'W');
    build_home(scope);
    build_marker(scope);
    build_rim(scope);
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

    s_photo_plane = lv_image_create(card);
    lv_image_set_src(s_photo_plane, &icons::plane_large_icon);
    lv_obj_set_style_image_recolor(s_photo_plane, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_image_recolor_opa(s_photo_plane, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(s_photo_plane, WAITING_PLANE_OPA, 0);
    lv_obj_set_pos(s_photo_plane, (INNER_W - icons::plane_large_icon.header.w) / 2,
                   (PHOTO_H - icons::plane_large_icon.header.h) / 2);
    lv_obj_set_hidden(s_photo_plane, true);
    quiet(s_photo_plane);
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

// Along the column's foot, wherever that is.
void place_readings_at(std::int32_t height)
{
    const std::int32_t name_y  = height - 2 * INSET - theme::type_label()->line_height;
    const std::int32_t value_y = name_y - theme::type_value()->line_height;
    for (const Row &row : s_rows) {
        lv_obj_set_y(row.value, value_y);
        lv_obj_set_y(row.name, name_y);
    }
}

void build_readings(lv_obj_t *card, std::int32_t height)
{
    for (int i = 0; i < READINGS; ++i) {
        s_rows[i].value = theme::make_label(card, NO_READING, theme::text, theme::type_value());
        quiet(s_rows[i].value);
        s_rows[i].name = theme::make_label(card, "", theme::secondary, theme::type_label());
        quiet(s_rows[i].name);
    }
    place_readings_at(height);
}

void build_column(lv_obj_t *parent, std::int32_t x, std::int32_t height)
{
    lv_obj_t *card = theme::make_card(parent);
    lv_obj_set_pos(card, x, 0);
    lv_obj_set_size(card, COLUMN_W, height);
    lv_obj_set_style_pad_all(card, INSET, 0);
    quiet(card);
    s_column = card;

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
    lv_obj_set_hidden(s_photo_plane, !waiting);
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
    plot.x = static_cast<std::int32_t>(std::lround(s_cx + plot.east_km * scale));
    plot.y = static_cast<std::int32_t>(std::lround(s_cy - plot.north_km * scale));

    const std::int32_t span = theme::text_width(blip_name(*plot.aircraft), marking_font());
    plot.label_x = plot.x > s_cx ? plot.x - LABEL_SIDE_GAP - span : plot.x + LABEL_SIDE_GAP;
    plot.label_w = span;
}

void mark_air(std::int32_t from, std::int32_t to, std::int32_t y)
{
    if (s_air_tiles.empty()) {
        return;
    }
    const std::size_t row = static_cast<std::size_t>(y / AIR_TILE) * s_tiles_w;
    std::fill(s_air_tiles.begin() + row + from / AIR_TILE, s_air_tiles.begin() + row + to / AIR_TILE + 1, 1);
    if (s_marking_way) {
        std::fill(s_way_tiles.begin() + row + from / AIR_TILE, s_way_tiles.begin() + row + to / AIR_TILE + 1, 1);
    }
}

void air_pixels(std::int32_t y, std::int32_t from, std::int32_t to, std::uint16_t ink)
{
    std::int32_t low = 0, high = 0;
    if (!air_span(y, low, high)) {
        return;
    }
    from = std::max(from, low);
    to   = std::min(to, high);
    if (to < from) {
        return;
    }
    mark_air(from, to, y);
    const std::size_t row = static_cast<std::size_t>(y) * s_ground_w;
    std::fill(s_frame + row + from, s_frame + row + to + 1, ink);
    std::memset(s_air_opa + row + from, LV_OPA_COVER, static_cast<std::size_t>(to - from + 1));
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
    if (s_frame == nullptr || count < MIN_POLYGON_POINTS) {
        return;
    }
    const Rows         rows   = rows_spanned(points, count);
    const std::int32_t top    = std::max<std::int32_t>(rows.top, 0);
    const std::int32_t bottom = std::min<std::int32_t>(rows.bottom, s_ground_h - 1);

    for (std::int32_t y = top; y <= bottom; ++y) {
        std::int32_t crossings[BLIP_MAX_CROSSINGS];
        const int    found = add_crossings(points, count, y, crossings, 0, BLIP_MAX_CROSSINGS);
        sort_crossings(crossings, found);
        for (int i = 0; i + 1 < found; i += 2) {
            const std::int32_t from = std::max<std::int32_t>(crossings[i], 0);
            const std::int32_t to   = std::min<std::int32_t>(crossings[i + 1], s_ground_w - 1);
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
        if (y < 0 || y >= s_ground_h) {
            continue;
        }
        const std::int32_t from = std::max<std::int32_t>(plot.x - DOT_SIZE / 2, 0);
        const std::int32_t to   = std::min<std::int32_t>(plot.x + DOT_SIZE / 2, s_ground_w - 1);
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
    const auto  x = static_cast<std::int32_t>(std::lround(s_cx + std::sin(radians) * reach));
    const auto  y = static_cast<std::int32_t>(std::lround(s_cy - std::cos(radians) * reach));

    lv_obj_set_pos(dot, x - RIM_SIZE / 2, y - RIM_SIZE / 2);
    theme::set_bg_color(dot, altitude_ink(aircraft.altitude_ft));
    lv_obj_set_hidden(dot, false);
}

// Each reading as wide as its value or its name, the three spread across the
// column: the quiet sky's names are longer than an aircraft's, and fixed
// columns ran them into each other.
void place_readings()
{
    lv_obj_update_layout(lv_obj_get_parent(s_rows[0].value));
    std::int32_t width[READINGS];
    std::int32_t used = 0;
    for (int i = 0; i < READINGS; ++i) {
        width[i] = std::max(lv_obj_get_width(s_rows[i].value), lv_obj_get_width(s_rows[i].name));
        used += width[i];
    }
    const std::int32_t gap = std::max(READING_MIN_GAP, (INNER_W - used) / (READINGS - 1));
    std::int32_t       x   = 0;
    for (int i = 0; i < READINGS; ++i) {
        lv_obj_set_x(s_rows[i].value, x);
        lv_obj_set_x(s_rows[i].name, x);
        x += width[i] + gap;
    }
}

void set_row(int index, const char *name, const char *value, std::uint32_t colour = theme::text)
{
    theme::set_text(s_rows[index].name, name);
    theme::set_text(s_rows[index].value, value);
    theme::set_text_color(s_rows[index].value, colour);
    place_readings();
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
        // From where it is, as its trail and the map are placed: by the feed's
        // bearing and distance it sat beside its own trail, the further out
        // the more.
        float east_km = 0.0f, north_km = 0.0f;
        flat_km(aircraft.lat, aircraft.lon, east_km, north_km);
        const float away_km = std::hypot(east_km, north_km);
        if (s_to_edges) {
            const float scale = static_cast<float>(s_radius) / range_km;
            const auto  x     = static_cast<std::int32_t>(std::lround(static_cast<float>(s_cx) + east_km * scale));
            const auto  y     = static_cast<std::int32_t>(std::lround(static_cast<float>(s_cy) - north_km * scale));
            if (!within_scope(x, y)) {
                continue;
            }
        } else if (away_km > range_km) {
            if (rim_used < RIM_DOTS) {
                draw_rim(s_rim[rim_used++], aircraft);
            }
            continue;
        }
        s_plots[s_shown].aircraft = &s_last->list[i];
        s_plots[s_shown].east_km  = east_km;
        s_plots[s_shown].north_km = north_km;
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
    // Only when the accent changed it: setting the same colour again draws the
    // whole square round the scope again.
    for (int i = 0; i < LEGEND_STEPS; ++i) {
        const lv_color_t ink = lv_color_hex(altitude_ink(SCALE_TOP_FT * i / (LEGEND_STEPS - 1)));
        if (!lv_color_eq(lv_obj_get_style_arc_color(s_legend[i], LV_PART_MAIN), ink)) {
            lv_obj_set_style_arc_color(s_legend[i], ink, LV_PART_MAIN);
        }
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
        await_picture();
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

// The chosen aircraft's way: where the feed has seen it, fading as it goes
// back, and a faint dashed line on from it to where it is going and back from
// where its trail begins to where it came from, each to the scope's edge.
constexpr lv_opa_t     TRAIL_OLD_OPA  = LV_OPA_20;
constexpr lv_opa_t     TRAIL_NEW_OPA  = LV_OPA_90;
constexpr lv_opa_t     ROUTE_OPA      = LV_OPA_40;
constexpr std::int32_t ROUTE_DASH     = 6;   // pixels drawn, then as many left
constexpr std::int32_t TRAIL_WIDTH    = 2;

void air_blend(std::int32_t x, std::int32_t y, std::uint16_t ink, lv_opa_t opa)
{
    for (std::int32_t dy = 0; dy < TRAIL_WIDTH; ++dy) {
        for (std::int32_t dx = 0; dx < TRAIL_WIDTH; ++dx) {
            if (!within_scope(x + dx, y + dy)) {
                continue;
            }
            const std::size_t at = static_cast<std::size_t>(y + dy) * s_ground_w + x + dx;
            mark_air(x + dx, x + dx, y + dy);
            if (opa > s_air_opa[at]) {
                s_frame[at]   = blend565(s_ground[at], ink, opa);
                s_air_opa[at] = opa;
            }
        }
    }
}

void trail_line(lv_point_t from, lv_point_t to, std::uint16_t ink, lv_opa_t opa)
{
    const std::int32_t dx     = std::abs(to.x - from.x);
    const std::int32_t dy     = -std::abs(to.y - from.y);
    const std::int32_t step_x = from.x < to.x ? 1 : -1;
    const std::int32_t step_y = from.y < to.y ? 1 : -1;
    std::int32_t       error  = dx + dy;
    for (;;) {
        air_blend(from.x, from.y, ink, opa);
        if (from.x == to.x && from.y == to.y) {
            return;
        }
        const std::int32_t twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            from.x += step_x;
        }
        if (twice <= dx) {
            error += dx;
            from.y += step_y;
        }
    }
}

// From `from` toward the point `towards`, stopping there or where the scope ends.
void route_line(lv_point_t from, float towards_x, float towards_y, std::uint16_t ink)
{
    const float dx     = towards_x - static_cast<float>(from.x);
    const float dy     = towards_y - static_cast<float>(from.y);
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1.0f) {
        return;
    }
    const float reach = std::min(length, static_cast<float>(2 * std::max({s_radius, s_ground_w, s_ground_h})));
    for (std::int32_t step = 0; step < static_cast<std::int32_t>(reach); ++step) {
        const auto x = static_cast<std::int32_t>(std::lround(from.x + dx * static_cast<float>(step) / length));
        const auto y = static_cast<std::int32_t>(std::lround(from.y + dy * static_cast<float>(step) / length));
        if (!within_scope(x, y)) {
            return;
        }
        if ((step / ROUTE_DASH) % 2 == 0) {
            air_blend(x, y, ink, ROUTE_OPA);
        }
    }
}

struct Placed {
    float x;
    float y;
};

Placed place_at(float lat, float lon, float range_km)
{
    const float scale = static_cast<float>(s_radius) / range_km;
    float       east = 0.0f, north = 0.0f;
    flat_km(lat, lon, east, north);
    return {static_cast<float>(s_cx) + east * scale, static_cast<float>(s_cy) - north * scale};
}

lv_point_t pixel_of(const Placed &at)
{
    return {static_cast<std::int32_t>(std::lround(at.x)), static_cast<std::int32_t>(std::lround(at.y))};
}

// A chosen aircraft's trail grows back from it along where it flew, rather
// than showing whole: from nothing when it is chosen, and from what showed
// when much more of it comes at once, as its past track does. A little more,
// as each reading adds, just shows.
constexpr std::uint32_t TRAIL_GROW_MS  = 700;
constexpr float         TRAIL_JUMP_PX  = 24.0f;
constexpr std::uint32_t TRAIL_FRAME_MS = 33;

char          s_grown_hex[radar::kHexLen] = {};  // whose trail s_grown_px measures
float         s_grown_px                  = 0.0f;  // shown, back from the aircraft
float         s_grow_from_px              = 0.0f;
std::uint32_t s_grow_at                   = 0;
bool          s_growing                   = false;
lv_timer_t   *s_grow_timer                = nullptr;
bool          s_named[DRAWN_MAX]          = {};  // the labels the last full redraw chose

float ease_out(float t)
{
    const float left = 1.0f - t;
    return 1.0f - left * left * left;
}

// How much of a trail `length_px` long to draw now, and whether to keep going.
float trail_reveal(const char *hex, float length_px)
{
    if (std::strcmp(hex, s_grown_hex) != 0) {
        std::snprintf(s_grown_hex, sizeof(s_grown_hex), "%s", hex);
        s_grown_px = 0.0f;
        s_growing  = false;
    }
    if (!s_growing && length_px - s_grown_px > TRAIL_JUMP_PX) {
        s_growing      = true;
        s_grow_from_px = s_grown_px;
        s_grow_at      = lv_tick_get();
        if (s_grow_timer != nullptr) {
            lv_timer_resume(s_grow_timer);
        }
    }
    if (!s_growing) {
        s_grown_px = length_px;
        return length_px;
    }
    const float t = std::min(static_cast<float>(lv_tick_elaps(s_grow_at)) / TRAIL_GROW_MS, 1.0f);
    s_growing     = t < 1.0f;
    s_grown_px    = s_grow_from_px + (length_px - s_grow_from_px) * ease_out(t);
    return s_grown_px;
}

float distance(lv_point_t a, lv_point_t b)
{
    const auto dx = static_cast<float>(b.x - a.x);
    const auto dy = static_cast<float>(b.y - a.y);
    return std::sqrt(dx * dx + dy * dy);
}

void draw_way(const Plot &plot)
{
    if (s_frame == nullptr) {
        return;
    }
    const float         range_km = shown_range_km();
    const std::uint16_t ink      = lv_color_to_u16(lv_color_hex(altitude_ink(plot.aircraft->altitude_ft)));
    const std::uint16_t faint    = lv_color_to_u16(lv_color_hex(theme::secondary));
    const lv_point_t    here{plot.x, plot.y};

    // Oldest first, and last the blip itself, which is placed a little
    // differently, by bearing and distance.
    radar::TrailPoint points[radar::kTrailPoints];
    const int         count = radar::trail(plot.aircraft->hex, points, radar::kTrailPoints);
    lv_point_t        at[radar::kTrailPoints + 1];
    for (int i = 0; i < count; ++i) {
        at[i] = pixel_of(place_at(points[i].lat, points[i].lon, range_km));
    }
    at[count] = here;
    // As far as it shows: past the scope's edge a longer trail would grow
    // unseen, and what shows would hurry.
    float length_px = 0.0f;
    for (int i = count; i > 0; --i) {
        length_px += distance(at[i], at[i - 1]);
        if (!within_scope(at[i - 1].x, at[i - 1].y)) {
            break;
        }
    }

    // Back from the aircraft, as far as has grown.
    const float reveal = trail_reveal(plot.aircraft->hex, length_px);
    float       drawn  = 0.0f;
    for (int i = count; i > 0 && drawn < reveal; --i) {
        const float leg = distance(at[i], at[i - 1]);
        lv_point_t  to  = at[i - 1];
        if (drawn + leg > reveal && leg > 0.0f) {
            const float part = (reveal - drawn) / leg;
            to = {at[i].x + static_cast<std::int32_t>(std::lround((at[i - 1].x - at[i].x) * part)),
                  at[i].y + static_cast<std::int32_t>(std::lround((at[i - 1].y - at[i].y) * part))};
        }
        const auto opa = static_cast<lv_opa_t>(TRAIL_OLD_OPA + (TRAIL_NEW_OPA - TRAIL_OLD_OPA) * i / count);
        trail_line(to, at[i], ink, opa);
        drawn += leg;
    }

    const bool route_known = std::strcmp(s_details_hex, plot.aircraft->hex) == 0;
    if (route_known && s_details.has_dest_at) {
        const Placed dest = place_at(s_details.dest_lat, s_details.dest_lon, range_km);
        route_line(here, dest.x, dest.y, faint);
    }
    if (route_known && s_details.has_origin_at && !s_growing) {
        const Placed origin = place_at(s_details.origin_lat, s_details.origin_lon, range_km);
        route_line(at[0], origin.x, origin.y, faint);
    }
}

void clear_traffic()
{
    if (s_frame != nullptr) {
        if (s_air_wholesale || s_air_tiles.empty()) {
            std::memcpy(s_frame, s_ground, ground_pixels() * RGB565_BYTES_PER_PX);
            std::memset(s_air_opa, 0, ground_pixels());
        } else {
            restore_tiles(s_air_tiles);
        }
        s_last_tiles.swap(s_air_tiles);
        std::fill(s_air_tiles.begin(), s_air_tiles.end(), 0);
        std::fill(s_way_tiles.begin(), s_way_tiles.end(), 0);
    }
}

void restore_tiles(const std::vector<std::uint8_t> &tiles)
{
    {
        {
            for (std::int32_t ty = 0; ty < s_tiles_h; ++ty) {
                const std::uint8_t *row = tiles.data() + static_cast<std::size_t>(ty) * s_tiles_w;
                for (std::int32_t tx = 0; tx < s_tiles_w; ++tx) {
                    if (row[tx] == 0) {
                        continue;
                    }
                    std::int32_t run = tx;  // the tiles side by side, put back a row of pixels at a time
                    while (run + 1 < s_tiles_w && row[run + 1] != 0) {
                        ++run;
                    }
                    const std::int32_t x    = tx * AIR_TILE;
                    const std::int32_t span = std::min((run + 1) * AIR_TILE, s_ground_w) - x;
                    const std::int32_t end  = std::min((ty + 1) * AIR_TILE, s_ground_h);
                    for (std::int32_t y = ty * AIR_TILE; y < end; ++y) {
                        const std::size_t at = static_cast<std::size_t>(y) * s_ground_w + x;
                        std::memcpy(s_frame + at, s_ground + at, static_cast<std::size_t>(span) * RGB565_BYTES_PER_PX);
                        std::memset(s_air_opa + at, 0, static_cast<std::size_t>(span));
                    }
                    tx = run;
                }
            }
        }
    }
}

// From `first` on: the names a reading did not use. Each one used is left
// where it was if it has not moved, so nothing is drawn again for it.
void hide_labels(int first)
{
    for (int i = first; i < LABEL_MAX; ++i) {
        lv_obj_set_hidden(s_blips[i].label, true);
    }
}

// What this reading or the last drew, to be drawn again: runs of tiles along
// a row, run on down while the rows below have the same, or the whole picture
// when the map changed or the runs would be too many to keep apart.
std::uint32_t tile_sum(std::int32_t tx, std::int32_t ty)
{
    const std::int32_t x    = tx * AIR_TILE;
    const std::int32_t w    = std::min(AIR_TILE, s_ground_w - x);
    const std::int32_t end  = std::min((ty + 1) * AIR_TILE, s_ground_h);
    std::uint32_t      sum  = 2166136261u;  // FNV-1a, over the tile's pixels
    for (std::int32_t y = ty * AIR_TILE; y < end; ++y) {
        const std::uint16_t *row = s_frame + static_cast<std::size_t>(y) * s_ground_w + x;
        for (std::int32_t i = 0; i < w; ++i) {
            sum = (sum ^ row[i]) * 16777619u;
        }
    }
    return sum == 0 ? 1 : sum;
}

void invalidate_tiles(std::vector<std::uint8_t> &maybe);

void invalidate_air()
{
    if (s_canvas == nullptr) {
        return;
    }
    if (std::exchange(s_air_wholesale, false) || s_air_tiles.empty()) {
        std::fill(s_tile_sums.begin(), s_tile_sums.end(), 0);
        lv_obj_invalidate(s_canvas);
        return;
    }
    for (std::size_t at = 0; at < s_air_tiles.size(); ++at) {
        s_maybe_changed[at] = s_air_tiles[at] | s_last_tiles[at];
    }
    invalidate_tiles(s_maybe_changed);
}

// Of the tiles that may have changed, those that now hold anything else, to be
// drawn again.
void invalidate_tiles(std::vector<std::uint8_t> &maybe)
{
    const std::int64_t summing = esp_timer_get_time();
    for (std::size_t at = 0; at < maybe.size(); ++at) {
        if (maybe[at] == 0) {
            continue;
        }
        const std::uint32_t sum = tile_sum(static_cast<std::int32_t>(at % s_tiles_w),
                                           static_cast<std::int32_t>(at / s_tiles_w));
        if (sum == s_tile_sums[at]) {
            maybe[at] = 0;
        }
        s_tile_sums[at] = sum;
    }
    s_sum_us += esp_timer_get_time() - summing;
    struct Run {
        std::int32_t from, to, top, bottom;
    };
    Run  runs[AIR_AREAS_MAX];
    int  count = 0;
    const auto dirty = [&maybe](std::int32_t tx, std::int32_t ty) {
        return maybe[static_cast<std::size_t>(ty) * s_tiles_w + tx] != 0;
    };
    for (std::int32_t ty = 0; ty < s_tiles_h; ++ty) {
        for (std::int32_t tx = 0; tx < s_tiles_w; ++tx) {
            if (!dirty(tx, ty)) {
                continue;
            }
            const std::int32_t from = tx;
            while (tx + 1 < s_tiles_w && dirty(tx + 1, ty)) {
                ++tx;
            }
            Run *same = nullptr;
            for (int i = 0; i < count && same == nullptr; ++i) {
                if (runs[i].bottom == ty - 1 && runs[i].from == from && runs[i].to == tx) {
                    same = &runs[i];
                }
            }
            if (same != nullptr) {
                same->bottom = ty;
            } else if (count == AIR_AREAS_MAX) {
                lv_obj_invalidate(s_canvas);
                return;
            } else {
                runs[count++] = {from, tx, ty, ty};
            }
        }
    }
    lv_area_t at;
    lv_obj_get_coords(s_canvas, &at);
    for (int i = 0; i < count; ++i) {
        const lv_area_t area{at.x1 + runs[i].from * AIR_TILE, at.y1 + runs[i].top * AIR_TILE,
                             std::min(at.x1 + (runs[i].to + 1) * AIR_TILE, at.x2 + 1) - 1,
                             std::min(at.y1 + (runs[i].bottom + 1) * AIR_TILE, at.y2 + 1) - 1};
        lv_obj_invalidate_area(s_canvas, &area);
    }
}

// Returns the chosen aircraft when it is among those drawn.
const radar::Aircraft *draw_traffic(const bool *named)
{
    const radar::Aircraft *chosen   = nullptr;
    int                    labelled = 0;
    if (const int at = s_chosen[0] != '\0' ? plot_of(s_chosen) : -1; at >= 0) {
        s_marking_way = true;
        draw_way(s_plots[at]);  // under every blip
        s_marking_way = false;
    }
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
    hide_labels(labelled);
    invalidate_air();
    return chosen;
}

// Each frame of a trail growing: the air drawn again over the map, which
// stays. A zoom under way draws every frame itself.
// A frame of the chosen aircraft's trail growing: only the way changes, so
// only where it was and is now is put back and drawn again, with the planes
// over it. A trail drawn where a plane is stays under it: the plane's pixels
// are opaque air, which a trail does not draw over.
std::int64_t s_way_us = 0;  // for the bench: redrawing ways, all told

void redraw_way()
{
    const Accrue accrue{s_way_us};
    if (s_frame == nullptr || s_air_tiles.empty() || s_air_wholesale) {
        clear_traffic();
        draw_traffic(s_named);
        return;
    }
    std::copy(s_way_tiles.begin(), s_way_tiles.end(), s_maybe_changed.begin());
    restore_tiles(s_way_tiles);
    std::fill(s_way_tiles.begin(), s_way_tiles.end(), 0);
    if (const int at = s_chosen[0] != '\0' ? plot_of(s_chosen) : -1; at >= 0) {
        s_marking_way = true;
        draw_way(s_plots[at]);
        s_marking_way = false;
    }
    for (int i = 0; i < s_shown; ++i) {
        const Plot        &plot = s_plots[i];
        const std::int32_t tx   = std::clamp<std::int32_t>(plot.x / AIR_TILE, 0, s_tiles_w - 1);
        const std::int32_t ty   = std::clamp<std::int32_t>(plot.y / AIR_TILE, 0, s_tiles_h - 1);
        bool               near = false;  // a tile put back within a tile of it, which a blip reaches into
        for (std::int32_t y = std::max<std::int32_t>(ty - 1, 0); y <= std::min<std::int32_t>(ty + 1, s_tiles_h - 1) && !near; ++y) {
            for (std::int32_t x = std::max<std::int32_t>(tx - 1, 0); x <= std::min<std::int32_t>(tx + 1, s_tiles_w - 1) && !near; ++x) {
                near = s_maybe_changed[static_cast<std::size_t>(y) * s_tiles_w + x] != 0;
            }
        }
        if (near) {
            raster_blip(plot);
        }
    }
    for (std::size_t at = 0; at < s_way_tiles.size(); ++at) {
        s_maybe_changed[at] |= s_way_tiles[at];
    }
    invalidate_tiles(s_maybe_changed);
}

void grow_tick(lv_timer_t *timer)
{
    if (s_scope != nullptr && lv_anim_get(s_scope, zoom_step) == nullptr && !s_fast_zooming) {
        redraw_way();
    }
    if (!s_growing) {
        lv_timer_pause(timer);
    }
}

// Everything in the scope is laid out for one frame, so a view of another
// size builds it again, and draws the map again to fit.
void build_scope_in(const Frame &frame, std::int32_t x, std::int32_t y)
{
    const Timed timed{"scope built"};
    if (s_scope != nullptr) {
        lv_anim_delete(s_scope, nullptr);  // a zoom under way ends where it was going
        lv_obj_delete(s_scope);
        for (void *buffer : {static_cast<void *>(s_ground), static_cast<void *>(s_frame),
                             static_cast<void *>(s_land_mask), static_cast<void *>(s_air_opa),
                             static_cast<void *>(s_map_points), static_cast<void *>(s_row_from),
                             static_cast<void *>(s_row_to), static_cast<void *>(s_crossings),
                             static_cast<void *>(s_crossing_count)}) {
            heap_caps_free(buffer);
        }
        s_ground = s_frame = nullptr;
        s_land_mask = s_air_opa = s_crossing_count = nullptr;
        s_row_from = s_row_to = nullptr;
        s_crossings = nullptr;
        s_map_points            = nullptr;
        s_canvas                = nullptr;
    }
    s_shown_range = static_cast<float>(RANGES[s_range_step]);
    build_scope(s_bezel, frame, RANGES[s_range_step]);
    lv_obj_move_to_index(s_scope, 0);  // under the corners and the summary
    lv_obj_set_pos(s_scope, x, y);
    build_key(s_scope);
    if (map_located()) {
        draw_map(s_map_lat, s_map_lon, static_cast<float>(RANGES[s_range_step]));
    }
}

// Fullscreen the column floats over the map, on a darkened patch of it.
constexpr lv_opa_t FLOATING_COLUMN_OPA = LV_OPA_80;

void place_summary(std::int32_t area_w, std::int32_t height)
{
    const std::int32_t line = marking_font()->line_height;
    lv_obj_set_pos(s_summary, area_w - EDGE - ZOOM_D - theme::space::s - SUMMARY_W,
                   height - EDGE - (ZOOM_D - line) / 2 - line);
}

// The scope's card and the column beside it, as the page has them.
void lay_out_page()
{
    const std::int32_t area_w = s_page_w - COLUMN_W - COLUMN_GAP;
    const std::int32_t disc   = std::min(area_w, s_page_h) - 2 * BEZEL;
    lv_obj_set_parent(s_bezel, s_page);
    lv_obj_set_style_bg_opa(s_bezel, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_bezel, 0, 0);
    lv_obj_set_size(s_bezel, area_w, s_page_h);
    build_scope_in({disc, disc, disc / 2, disc / 2, disc / 2 - RIM_BAND, false, 0}, (area_w - disc) / 2,
                   (s_page_h - disc) / 2);
    place_corners(area_w, s_page_h);
    paint_chips(false);
    lv_obj_set_hidden(s_screw, false);
    place_summary(area_w, s_page_h);

    lv_obj_set_parent(s_column, s_page);
    lv_obj_set_pos(s_column, area_w + COLUMN_GAP, 0);
    lv_obj_set_size(s_column, COLUMN_W, s_page_h);
    lv_obj_set_style_bg_color(s_column, lv_color_hex(theme::panel_light), 0);
    lv_obj_set_style_bg_opa(s_column, LV_OPA_COVER, 0);
    place_readings_at(s_page_h);
    show_radar(*s_last);
}

// Fullscreen there are no cards: the map is one rounded panel in the screen's
// black margin, as the other fullscreen views sit, the planes to its every
// edge, the rings at its height beside the column, and the column floating
// over it on a darkened patch.
void lay_out_full()
{
    const detail::Layout l      = detail::layout();
    const std::int32_t   w      = l.screen_w - 2 * detail::GAP;
    const std::int32_t   h      = l.screen_h - 2 * detail::GAP;
    const std::int32_t   area_w = w - COLUMN_W - detail::GAP;  // the map beside the column
    lv_obj_set_parent(s_bezel, s_full);
    lv_obj_set_style_bg_opa(s_bezel, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(s_bezel, detail::GAP, detail::GAP);
    lv_obj_set_size(s_bezel, w, h);
    build_scope_in({w, h, area_w / 2, h / 2, h / 2 - RIM_BAND, true, theme::radius::card}, 0, 0);
    place_corners(area_w, h);
    paint_chips(true);
    lv_obj_set_hidden(s_screw, true);
    place_summary(area_w, h);

    lv_obj_set_parent(s_column, s_full);
    lv_obj_set_pos(s_column, detail::GAP + area_w, 2 * detail::GAP);
    lv_obj_set_size(s_column, COLUMN_W, h - 2 * detail::GAP);
    lv_obj_set_style_bg_color(s_column, lv_color_hex(theme::background), 0);
    lv_obj_set_style_bg_opa(s_column, FLOATING_COLUMN_OPA, 0);
    place_readings_at(h - 2 * detail::GAP);
    show_radar(*s_last);
}

bool full_open()
{
    return detail::view_open(s_full_view);
}

void open_full()
{
    detail::open_view(s_full_view);
}

void close_full()
{
    detail::close_view(s_full_view);
}

void full_clicked(lv_event_t *)
{
    detail::toggle_view(s_full_view);
}

// Over the whole screen, the rail and the tabs under it; the map and the
// column move onto it while it is up, and back onto the page after.
void build_full()
{
    const detail::Layout l = detail::layout();
    s_full = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(s_full, 0, 0);
    lv_obj_set_size(s_full, l.screen_w, l.screen_h);
    theme::style_panel(s_full, theme::background, 0);
    lv_obj_set_scrollable(s_full, false);  // clickable, so nothing under it is
    // Over the map, which moves onto the view after this as it opens.
    s_full_desk = detail::add_desk_shortcuts(s_full, FULL_DESK_INSET, FULL_DESK_INSET, theme::background);
    s_full_view = detail::add_view({"radar", detail::ViewKind::Fullscreen, s_full,
                                    [] {
                                        lay_out_full();
                                        lv_obj_move_foreground(s_full_desk);
                                        lv_obj_move_foreground(s_full_clock);
                                        lv_obj_move_foreground(s_full_badge);
                                        lv_image_set_src(s_full_mark, &icons::collapse_icon);
                                    },
                                    [] {
                                        lay_out_page();
                                        lv_image_set_src(s_full_mark, &icons::expand_icon);
                                    }});
    for (lv_obj_t *control : {s_full_desk, s_full_chip, s_zoom_out, s_zoom_in}) {
        detail::fade_when_idle(s_full_view, control);
    }
    const detail::ViewClock clock = detail::add_view_clock(s_full_view, s_full, s_full_chip);
    s_full_clock                  = clock.label;
    s_full_badge                  = clock.badge;
}
}  // namespace

bool radar_full_open()
{
    return full_open();
}

void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    s_last  = psram_array<radar::Snapshot>(1);
    s_blips = psram_array<Blip>(LABEL_MAX);
    s_plots = psram_array<Plot>(DRAWN_MAX);
    if (s_last == nullptr || s_blips == nullptr || s_plots == nullptr) {
        return;
    }
    s_last->age_s = -1;  // nothing read yet, rather than a reading that failed

    s_page        = page;
    s_page_w      = width;
    s_page_h      = height;
    s_shown_range = static_cast<float>(RANGES[s_range_step]);
    s_bezel       = theme::make_card(page);
    lv_obj_set_style_pad_all(s_bezel, 0, 0);
    quiet(s_bezel);
    build_corners(s_bezel);

    s_summary = theme::make_label(s_bezel, "", theme::amber, marking_font());
    lv_obj_set_width(s_summary, SUMMARY_W);
    lv_obj_set_style_text_align(s_summary, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_summary, marking_font()->line_height);
    quiet(s_summary);
    paint_range_buttons();
    build_column(page, 0, height);
    lay_out_page();
    build_full();
    s_grow_timer = lv_timer_create(grow_tick, TRAIL_FRAME_MS, nullptr);
    lv_timer_pause(s_grow_timer);
    detail::subscribe(detail::Topic::Radar, detail::kNoView, refresh_radar);
    // What was looked up for an aircraft, each part once as it comes.
    detail::subscribe(detail::Topic::Lookup, detail::kNoView, [] {
        static std::uint32_t      s_details_shown = 0;
        static std::uint32_t      s_photo_shown   = 0;
        const detail::RadarLookup &lookup         = detail::radar_lookup();
        if (lookup.details_stamp != s_details_shown) {
            s_details_shown = lookup.details_stamp;
            show_radar_details(lookup.details_hex, lookup.details);
        }
        if (lookup.photo_stamp != s_photo_shown) {
            s_photo_shown = lookup.photo_stamp;
            show_radar_photo(lookup.photo_hex, lookup.photo, lookup.photo_width, lookup.photo_height);
        }
    });
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
    if (s_ground_spent || s_fast_zooming) {
        return;  // a zoom is drawing it, over a map it draws as it goes; its end shows this
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
    std::copy(std::begin(named), std::end(named), std::begin(s_named));
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
        if (full_open()) {
            close_full();
        }
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
    // The photo comes on its own, before these or after: only a wait for
    // both becomes a wait for the photo.
    if (s_picture == Picture::Looking) {
        s_picture = Picture::Loading;
    }
    show_radar(*s_last);
}

#if REMOTE_ENABLED
namespace {
// One frame drawn and handed over now, in milliseconds.
int timed_frame()
{
    const std::int64_t began = esp_timer_get_time();
    lv_refr_now(nullptr);
    return static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs);
}

int whole_frame_ms()
{
    constexpr int RUNS  = 4;
    int           total = 0;
    for (int i = 0; i < RUNS; ++i) {
        lv_obj_invalidate(lv_screen_active());
        total += timed_frame();
    }
    return total / RUNS;
}

int without_ms(const std::vector<lv_obj_t *> &parts)
{
    for (lv_obj_t *part : parts) {
        lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
    const int ms = whole_frame_ms();
    for (lv_obj_t *part : parts) {
        lv_obj_remove_flag(part, LV_OBJ_FLAG_HIDDEN);
    }
    return ms;
}

// A zoom one step in or out as a tap starts it, run to its end: how many
// frames it showed, how far along each, and how long it all took.
void time_zoom(int step, const char *name, char *out, std::size_t size, int &n)
{
    constexpr std::int64_t GIVE_UP_US = 3 * units::kUsPerSecond;
    const int              next       = std::clamp(s_range_step + step, 0, RANGE_COUNT - 1);
    if (next == s_range_step) {
        return;
    }
    const int from = static_cast<int>(std::lround(shown_range_km()));
    s_range_step   = next;
    s_zoom_traced  = 0;
    board::take_flush_times();
    const std::int64_t began = esp_timer_get_time();
    apply_range(from);
    while ((lv_anim_get(s_scope, nullptr) != nullptr || s_fast_zooming) && esp_timer_get_time() - began < GIVE_UP_US) {
        lv_timer_handler();
    }
#ifdef ESP_PLATFORM
    if (s_fast_frames > 0) {
        const board::FlushTimes zoomed = board::take_flush_times();
        const std::int64_t      moved  = esp_timer_get_time() - began;
        lv_refr_now(nullptr);
        n += std::snprintf(out + n, size - n,
                           "zoom %s to %d km by the PPA: %d frames in %lld ms (magnifying %lld, waiting %lld, "
                           "catching up %lld), and the last drawn by %d ms\n",
                           name, RANGES[s_range_step], s_fast_frames, moved / 1000, zoomed.rotate_us / 1000,
                           zoomed.wait_us / 1000, zoomed.catch_up_us / 1000,
                           static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs));
        s_fast_frames = 0;
        return;
    }
#endif
    lv_refr_now(nullptr);
    const int took = static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs);
    n += std::snprintf(out + n, size - n, "zoom %s to %d km: %d frames in %d ms, at", name, RANGES[s_range_step],
                       s_zoom_traced, took);
    for (int i = 0; i < s_zoom_traced; ++i) {
        n += std::snprintf(out + n, size - n, " %d%%", static_cast<int>(s_zoom_trace[i] * 100 / ZOOM_PROGRESS_FULL));
    }
    n += std::snprintf(out + n, size - n, "\n");
    s_zoom_traced = -1;
}

// What the bench found before it showed the radar, put back once it is done.
struct BenchLeft {
    bool held       = false;
    bool gated      = true;
    int  page       = 0;
    int  range_step = 0;
    bool full       = false;
};
BenchLeft s_bench_left;

void bench_put_back()
{
    if (!s_bench_left.held) {
        return;
    }
    if (s_range_step != s_bench_left.range_step) {
        s_range_step = s_bench_left.range_step;
        apply_range(0);
    }
    if (s_bench_left.full != full_open()) {
        s_bench_left.full ? open_full() : close_full();
    }
    detail::s_presence_gate = s_bench_left.gated;
    detail::select_page(s_bench_left.page);
    s_bench_left.held = false;
}

// The radar page shown, even with the phone away, so its feed runs; ready once
// the feed has answered and the map is drawn.
bool bench_ready()
{
    if (!s_bench_left.held) {
        s_bench_left = {true, detail::s_presence_gate, detail::s_page, s_range_step, full_open()};
    }
    detail::s_presence_gate = false;
    if (full_open()) {
        close_full();
    }
    if (detail::s_page != detail::RADAR_PAGE) {
        detail::select_page(detail::RADAR_PAGE);
    }
    return s_scope != nullptr && s_last != nullptr && s_last->ok && map_located();
}
}  // namespace

int bench_radar_full(char *out, std::size_t size, bool open)
{
    if (!open) {
#ifdef ESP_PLATFORM
        if (s_fast_zooming) {
            s_fast_zooming = false;
            board::zoom_end();
            lv_display_enable_invalidation(lv_display_get_default(), true);
            lv_obj_invalidate(lv_screen_active());
        }
#endif
        bench_put_back();
        return std::snprintf(out, size, "put back\n");
    }
    if (!bench_ready()) {
        return std::snprintf(out, size, "waiting for the feed\n");
    }
    open_full();
    s_following    = true;
    s_grown_hex[0] = '\0';  // its trail grows in, as when chosen
    show_radar(*s_last);
    return std::snprintf(out, size, "open\n");
}

// One frame of a zoom in, magnified by a quarter, held on the panel until
// bench_radar_full(false) puts the screen back: to see what a zoom shows.
int bench_radar_zoom_frame(char *out, std::size_t size)
{
#ifdef ESP_PLATFORM
    if (!full_open() || s_frame == nullptr) {
        return std::snprintf(out, size, "not open\n");
    }
    lv_refr_now(nullptr);
    lv_area_t keep[16];
    int       kept = 0;
    for (lv_obj_t *control : {s_zoom_out, s_zoom_in, s_full_chip, s_full_desk, s_full_clock, s_full_badge, s_rings[0],
                              s_rings[1], s_rings[2], s_rings[3], s_compass[0], s_compass[1], s_compass[2], s_compass[3]}) {
        if (control != nullptr && !lv_obj_has_flag(control, LV_OBJ_FLAG_HIDDEN) && kept < 16) {
            lv_obj_get_coords(control, &keep[kept++]);
        }
    }
    board::zoom_begin(keep, kept);
    take_rings_out();
    lv_display_enable_invalidation(lv_display_get_default(), false);
    s_fast_zooming = true;
    s_fast_from = s_fast_picture = RANGES[s_range_step];
    s_fast_to   = static_cast<int>(static_cast<float>(s_fast_from) / 1.25f);
    show_magnified(static_cast<std::int32_t>(std::lround(1.25f * 16.0f)));
    return std::snprintf(out, size, "held\n");
#else
    return std::snprintf(out, size, "panel only\n");
#endif
}

int bench_radar_open(char *out, std::size_t size)
{
    return std::snprintf(out, size, bench_ready() ? "ready\n" : "waiting for the feed\n");
}

// For a development build's /bench page, with the LVGL lock: opening the
// fullscreen radar, drawing it whole and without each of its parts, zooming it
// both ways and closing it. Afterwards the page, the range and the gate are as
// they were before bench_radar_open.
int bench_radar(char *out, std::size_t size)
{
    if (!bench_ready()) {
        return std::snprintf(out, size, "waiting for the feed\n");
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(nullptr);

    int n = std::snprintf(out, size, "page frame %d ms\n", whole_frame_ms());
    const std::int64_t opening = esp_timer_get_time();
    open_full();
    const int laid_out = static_cast<int>((esp_timer_get_time() - opening) / units::kUsPerMs);
    const int first    = timed_frame();
    n += std::snprintf(out + n, size - n, "opening fullscreen: laid out %d ms, first frame %d ms\n", laid_out, first);
    board::take_flush_times();
    const int whole = whole_frame_ms();
    const board::FlushTimes flushed = board::take_flush_times();
    const auto per_frame = [&](std::int64_t us) {
        return static_cast<double>(us) / units::kUsPerMs / std::max<std::uint32_t>(flushed.frames, 1);
    };
    n += std::snprintf(out + n, size - n,
                       "whole %d ms: %u areas, %llu kpx a frame; turning onto the panel %.1f ms, waiting for it "
                       "%.1f ms, catching up %.1f ms, the rest drawing\n",
                       whole, static_cast<unsigned>(flushed.areas / std::max<std::uint32_t>(flushed.frames, 1)),
                       static_cast<unsigned long long>(flushed.pixels / 1000 / std::max<std::uint32_t>(flushed.frames, 1)),
                       per_frame(flushed.rotate_us), per_frame(flushed.wait_us), per_frame(flushed.catch_up_us));
    const auto line = [&](const char *name, int ms) {
        n += std::snprintf(out + n, size - n, "without %s %d ms\n", name, ms);
    };
    line("the picture", without_ms({s_canvas}));
    line("the column", without_ms({s_column}));
    line("the photo", without_ms({s_photo_frame}));
    std::vector<lv_obj_t *> labels;
    for (int i = 0; i < LABEL_MAX; ++i) {
        labels.push_back(s_blips[i].label);
    }
    for (lv_obj_t *ring_label : s_rings) {
        labels.push_back(ring_label);
    }
    line("the names", without_ms(labels));
    line("the altitude scale", without_ms({std::begin(s_legend), std::end(s_legend)}));
    line("the whole scope", without_ms({s_scope}));

    const auto average = [](auto &&frame) {
        constexpr int RUNS  = 8;
        int           total = 0;
        for (int i = 0; i < RUNS; ++i) {
            const std::int64_t began = esp_timer_get_time();
            frame();
            lv_refr_now(nullptr);
            total += static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs);
        }
        return total / RUNS;
    };
    lv_refr_now(nullptr);
    {
        constexpr int RUNS = 8;
        std::int64_t  cpu = 0, screen = 0;
        board::take_flush_times();
        lv_refr_now(nullptr);
        std::int64_t restoring = 0, drawing = 0, summing = 0;
        for (int i = 0; i < RUNS; ++i) {
            const std::int64_t began = esp_timer_get_time();
            clear_traffic();
            const std::int64_t cleared = esp_timer_get_time();
            const std::int64_t before_sums = s_sum_us;
            draw_traffic(s_named);
            const std::int64_t drawn = esp_timer_get_time();
            restoring += cleared - began;
            summing += s_sum_us - before_sums;
            drawing += drawn - cleared - (s_sum_us - before_sums);
            lv_refr_now(nullptr);
            cpu += drawn - began;
            screen += esp_timer_get_time() - drawn;
        }
        const board::FlushTimes flushed = board::take_flush_times();
        const std::uint32_t frames = std::max<std::uint32_t>(flushed.frames, 1);
        n += std::snprintf(out + n, size - n,
                           "a trail frame, nothing moved: the air %lld ms, the screen %lld ms, %u areas, %llu kpx; "
                           "turning %.1f ms, waiting %.1f ms, catching up %.1f ms\n"
                           "  the air: putting the map back %lld ms, drawing %lld ms, comparing tiles %lld ms\n",
                           cpu / RUNS / 1000, screen / RUNS / 1000, static_cast<unsigned>(flushed.areas / frames),
                           static_cast<unsigned long long>(flushed.pixels / 1000 / frames),
                           static_cast<double>(flushed.rotate_us) / 1000.0 / frames,
                           static_cast<double>(flushed.wait_us) / 1000.0 / frames,
                           static_cast<double>(flushed.catch_up_us) / 1000.0 / frames, restoring / RUNS / 1000,
                           drawing / RUNS / 1000, summing / RUNS / 1000);
        lv_display_t *disp = lv_display_get_default();
        clear_traffic();
        draw_traffic(s_named);
        n += std::snprintf(out + n, size - n, "its areas:");
        for (std::uint32_t i = 0; i < disp->inv_p && i < 24; ++i) {
            const lv_area_t &a = disp->inv_areas[i];
            n += std::snprintf(out + n, size - n, " %ldx%ld@%ld,%ld%s", static_cast<long>(lv_area_get_width(&a)),
                               static_cast<long>(lv_area_get_height(&a)), static_cast<long>(a.x1), static_cast<long>(a.y1),
                               disp->inv_area_joined[i] ? "j" : "");
        }
        n += std::snprintf(out + n, size - n, "\n");
        lv_refr_now(nullptr);
    }
    {
        board::take_flush_times();
        const int ms = average([] { redraw_way(); });
        const board::FlushTimes flushed = board::take_flush_times();
        n += std::snprintf(out + n, size - n, "a growing trail's frame %d ms, %u areas\n", ms,
                           static_cast<unsigned>(flushed.areas / std::max<std::uint32_t>(flushed.frames, 1)));
    }
    n += std::snprintf(out + n, size - n, "a reading shown again %d ms\n", average([] { show_radar(*s_last); }));
    if (s_chosen[0] != '\0') {
        // A trail growing in again from nothing, as when an aircraft is chosen.
        constexpr std::int64_t GIVE_UP_US = 2 * units::kUsPerSecond;
        struct Frame {
            std::int64_t start, rendered, done;
            std::int32_t areas;
        };
        static Frame  frames[40];
        static int    frame_count = 0;
        static Frame *open_frame  = nullptr;
        frame_count               = 0;
        lv_display_t *disp        = lv_display_get_default();
        const auto    on_refr     = [](lv_event_t *e) {
            const std::int64_t now = esp_timer_get_time();
            switch (lv_event_get_code(e)) {
                case LV_EVENT_REFR_START:
                    open_frame = frame_count < 40 ? &frames[frame_count++] : nullptr;
                    if (open_frame != nullptr) {
                        *open_frame = {now, 0, 0, static_cast<std::int32_t>(lv_display_get_default()->inv_p)};
                    }
                    break;
                case LV_EVENT_RENDER_READY:
                    if (open_frame != nullptr) {
                        open_frame->rendered = now;
                    }
                    break;
                case LV_EVENT_REFR_READY:
                    if (open_frame != nullptr) {
                        open_frame->done = now;
                    }
                    break;
                default:
                    break;
            }
        };
        lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_START, nullptr);
        lv_display_add_event_cb(disp, on_refr, LV_EVENT_RENDER_READY, nullptr);
        lv_display_add_event_cb(disp, on_refr, LV_EVENT_REFR_READY, nullptr);
        s_grown_hex[0] = '\0';
        lv_refr_now(nullptr);
        frame_count = 0;
        board::take_flush_times();
        s_way_us = 0;
        const std::int64_t began = esp_timer_get_time();
        show_radar(*s_last);
        while ((s_growing || lv_display_get_default()->inv_p > 0) && esp_timer_get_time() - began < GIVE_UP_US) {
            lv_timer_handler();
        }
        lv_display_remove_event_cb_with_user_data(disp, on_refr, nullptr);
        n += std::snprintf(out + n, size - n, "  frames (start ms: areas, drawn by, done by):");
        for (int i = 0; i < frame_count && n < static_cast<int>(size) - 64; ++i) {
            n += std::snprintf(out + n, size - n, " %lld:%ld,%lld,%lld", (frames[i].start - began) / 1000,
                               static_cast<long>(frames[i].areas), (frames[i].rendered - frames[i].start) / 1000,
                               (frames[i].done - frames[i].start) / 1000);
        }
        n += std::snprintf(out + n, size - n, "\n");
        const int took = static_cast<int>((esp_timer_get_time() - began) / units::kUsPerMs);
        const board::FlushTimes grown = board::take_flush_times();
        n += std::snprintf(out + n, size - n, "a trail growing in: %u frames in %d ms, %.0f a second\n",
                           static_cast<unsigned>(grown.frames), took, grown.frames * 1000.0 / std::max(took, 1));
        n += std::snprintf(out + n, size - n,
                           "  in all: the way %lld ms, turning %lld ms, waiting %lld ms, catching up %lld ms, %u areas %llu kpx\n",
                           s_way_us / 1000, grown.rotate_us / 1000, grown.wait_us / 1000, grown.catch_up_us / 1000,
                           static_cast<unsigned>(grown.areas), static_cast<unsigned long long>(grown.pixels / 1000));
    }
    {
        lv_display_t *disp = lv_display_get_default();
        show_radar(*s_last);
        n += std::snprintf(out + n, size - n, "its areas:");
        for (std::uint32_t i = 0; i < disp->inv_p && i < 24; ++i) {
            const lv_area_t &a = disp->inv_areas[i];
            n += std::snprintf(out + n, size - n, " %ldx%ld@%ld,%ld%s", static_cast<long>(lv_area_get_width(&a)),
                               static_cast<long>(lv_area_get_height(&a)), static_cast<long>(a.x1), static_cast<long>(a.y1),
                               disp->inv_area_joined[i] ? "j" : "");
        }
        n += std::snprintf(out + n, size - n, "\n");
        lv_refr_now(nullptr);
    }

    {
        // One tile of the picture drawn again, as the air's frames are, with
        // each part of the view taken away in turn: what an area costs.
        lv_area_t at;
        lv_obj_get_coords(s_canvas, &at);
        const lv_area_t tile{at.x1 + 320, at.y1 + 320, at.x1 + 351, at.y1 + 351};
        const auto tile_ms = [&tile] {
            constexpr int RUNS  = 8;
            std::int64_t  total = 0;
            lv_refr_now(nullptr);
            for (int i = 0; i < RUNS; ++i) {
                lv_obj_invalidate_area(s_canvas, &tile);
                const std::int64_t began = esp_timer_get_time();
                lv_refr_now(nullptr);
                total += esp_timer_get_time() - began;
            }
            return static_cast<double>(total) / RUNS / 1000.0;
        };
        board::take_flush_times();
        n += std::snprintf(out + n, size - n, "a 32 px tile: %.1f ms", tile_ms());
        const board::FlushTimes one = board::take_flush_times();
        n += std::snprintf(out + n, size - n, " (turning %.1f, waiting %.1f, catching up %.1f)\n",
                           static_cast<double>(one.rotate_us) / 1000.0 / std::max<std::uint32_t>(one.frames, 1),
                           static_cast<double>(one.wait_us) / 1000.0 / std::max<std::uint32_t>(one.frames, 1),
                           static_cast<double>(one.catch_up_us) / 1000.0 / std::max<std::uint32_t>(one.frames, 1));
        const auto tile_without = [&](const char *name, std::vector<lv_obj_t *> parts) {
            for (lv_obj_t *part : parts) {
                lv_obj_add_flag(part, LV_OBJ_FLAG_HIDDEN);
            }
            n += std::snprintf(out + n, size - n, "  without %s %.1f ms\n", name, tile_ms());
            for (lv_obj_t *part : parts) {
                lv_obj_remove_flag(part, LV_OBJ_FLAG_HIDDEN);
            }
        };
        // The same area drawn again through the view's root rather than the
        // picture, so it is drawn with and without the picture under it.
        const auto root_ms = [&tile] {
            constexpr int RUNS  = 8;
            std::int64_t  total = 0;
            lv_refr_now(nullptr);
            for (int i = 0; i < RUNS; ++i) {
                lv_obj_invalidate_area(s_full, &tile);
                lv_refr_now(nullptr);  // the wait for the panel is in each, so it is taken out
                const std::int64_t began = esp_timer_get_time();
                lv_obj_invalidate_area(s_full, &tile);
                board::take_flush_times();
                lv_refr_now(nullptr);
                const board::FlushTimes one = board::take_flush_times();
                total += esp_timer_get_time() - began - one.wait_us;
            }
            return static_cast<double>(total) / RUNS / 1000.0;
        };
        n += std::snprintf(out + n, size - n, "  the tile through the root, less the wait: %.1f ms", root_ms());
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        n += std::snprintf(out + n, size - n, ", without the picture %.1f ms", root_ms());
        lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_scope, LV_OBJ_FLAG_HIDDEN);
        n += std::snprintf(out + n, size - n, ", without the scope %.1f ms\n", root_ms());
        lv_obj_remove_flag(s_scope, LV_OBJ_FLAG_HIDDEN);
        tile_without("the altitude scale", {std::begin(s_legend), std::end(s_legend)});
        tile_without("the picture", {s_canvas});
        std::vector<lv_obj_t *> names;
        for (int i = 0; i < LABEL_MAX; ++i) {
            names.push_back(s_blips[i].label);
        }
        tile_without("the names", names);
        tile_without("the column", {s_column});
        tile_without("the whole scope", {s_scope});
    }

    const int start_step = s_range_step;
    time_zoom(start_step > 0 ? -1 : 1, start_step > 0 ? "in" : "out", out, size, n);
    time_zoom(start_step > 0 ? 1 : -1, start_step > 0 ? "out" : "in", out, size, n);

    const std::int64_t closing = esp_timer_get_time();
    close_full();
    lv_refr_now(nullptr);
    n += std::snprintf(out + n, size - n, "closing fullscreen %d ms\n",
                       static_cast<int>((esp_timer_get_time() - closing) / units::kUsPerMs));
    bench_put_back();
    return n;
}
#endif

bool radar_choose(const char *hex)
{
    const int at = plot_of(hex);
    if (at < 0) {
        return false;
    }
    s_following = false;
    choose(*s_plots[at].aircraft);
    show_radar(*s_last);
    return true;
}

}  // namespace ui
