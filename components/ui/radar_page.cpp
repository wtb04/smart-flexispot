#include "radar_page.h"

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

// Where the altitude scale beside the scope tops out.
constexpr int SCALE_TOP_FT = 40000;

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

Blip               s_blips[radar::kMaxAircraft] = {};
lv_obj_t          *s_rim[RIM_DOTS]              = {};
lv_point_precise_t s_spokes[SPOKES][2]          = {};

lv_obj_t *s_title      = nullptr;
lv_obj_t *s_operator   = nullptr;
lv_obj_t *s_airframe   = nullptr;
lv_obj_t *s_route      = nullptr;
lv_obj_t *s_cities     = nullptr;
lv_obj_t *s_summary    = nullptr;
lv_obj_t *s_photo      = nullptr;
lv_obj_t *s_photo_note = nullptr;
lv_obj_t *s_nearby     = nullptr;

lv_image_dsc_t s_photo_dsc     = {};
Row            s_rows[READINGS] = {};

// Held against the selection so a lookup that comes back after the tap has
// moved on is thrown away rather than shown against the wrong aircraft.
radar::Details s_details                     = {};
char           s_details_hex[radar::kHexLen] = {};

// Held so the panel can be redrawn when the selection changes rather than only
// when a reading lands.
radar::Snapshot s_last                   = {};
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

Plot s_plots[radar::kMaxAircraft];
int  s_shown  = 0;
int  s_beyond = 0;

// Height by brightness inside the panel's own palette, rather than by hue. The
// map projects use a full rainbow, and borrowing it put cyan and violet on a
// screen that is otherwise warm orange on near-black: readable, and wrong.
struct Stop {
    int           feet;
    std::uint32_t ink;
};

constexpr Stop ALTITUDE_INK[] = {
    {0, 0x8a4a1e},      // dim, so low traffic sits back
    {12000, theme::orange},
    {25000, theme::amber},
    {40000, theme::text},  // pale cream at cruise
};

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
    const int count = static_cast<int>(std::size(ALTITUDE_INK));
    for (int i = 1; i < count; ++i) {
        if (feet <= ALTITUDE_INK[i].feet) {
            const float span = static_cast<float>(ALTITUDE_INK[i].feet - ALTITUDE_INK[i - 1].feet);
            const float frac = static_cast<float>(feet - ALTITUDE_INK[i - 1].feet) / span;
            return mix(ALTITUDE_INK[i - 1].ink, ALTITUDE_INK[i].ink, frac);
        }
    }
    return ALTITUDE_INK[count - 1].ink;
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
        char text[8];
        std::snprintf(text, sizeof(text), "%d", range_km * i / RINGS);
        lv_obj_t *label = theme::make_label(scope, text, theme::secondary, fonts::size_16());
        lv_obj_set_style_text_opa(label, LV_OPA_40, 0);
        lv_obj_set_pos(label, s_centre + 8, s_centre - radius - fonts::size_16()->line_height - 2);
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
    show_radar(s_last);
}

// The scope's background is clipped to a circle, so its square's bottom-left
// corner is space nothing can reach. The scale that reads the blips goes there,
// on the page behind it, rather than taking room in the column.
void build_legend(lv_obj_t *parent, std::int32_t side)
{
    constexpr std::int32_t BAR_W = 132;
    constexpr std::int32_t BAR_H = 8;
    constexpr int          STEPS = 22;

    const std::int32_t x    = 8;
    const std::int32_t y    = side - 46;
    const std::int32_t step = BAR_W / STEPS;

    for (int i = 0; i < STEPS; ++i) {
        lv_obj_t *segment = lv_obj_create(parent);
        lv_obj_set_size(segment, step + 1, BAR_H);
        lv_obj_set_pos(segment, x + i * step, y);
        theme::style_panel(segment, altitude_ink(SCALE_TOP_FT * i / (STEPS - 1)), 0);
        quiet(segment);
    }

    lv_obj_t *low = theme::make_label(parent, "0", theme::secondary, fonts::size_16());
    lv_obj_set_style_text_opa(low, LV_OPA_50, 0);
    lv_obj_set_pos(low, x, y + BAR_H + 3);
    quiet(low);

    lv_obj_t *high = theme::make_label(parent, "40 000 ft", theme::secondary, fonts::size_16());
    lv_obj_set_style_text_opa(high, LV_OPA_50, 0);
    lv_obj_set_pos(high, x + BAR_W - text_width("40 000 ft", fonts::size_16()), y + BAR_H + 3);
    quiet(high);
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

    compass(scope, "N", 'N');
    compass(scope, "S", 'S');
    compass(scope, "E", 'E');
    compass(scope, "W", 'W');

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

    s_title     = stacked(column, 0, theme::orange, fonts::size_32());
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

    s_photo = lv_image_create(column);
    lv_obj_set_pos(s_photo, 0, photo_y);
    lv_obj_set_size(s_photo, COLUMN_W, photo_h);
    lv_image_set_inner_align(s_photo, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_set_style_radius(s_photo, 10, 0);
    lv_obj_set_style_clip_corner(s_photo, true, 0);
    lv_obj_set_hidden(s_photo, true);
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
    lv_obj_set_hidden(s_photo, s_picture != Picture::Shown);
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
    // was most of the clutter.
    const char *shape = aircraft.desc[0] != '\0'              ? aircraft.desc
                        : (mine && s_details.model[0] != '\0') ? s_details.model
                                                               : aircraft.type;
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

    theme::set_text(s_summary, shout != nullptr ? shout : (s_last.ok ? "" : "feed unreachable"));
    theme::set_text_color(s_summary, shout != nullptr ? theme::red : theme::amber);
}

}  // namespace

void build_radar_page(lv_obj_t *page, std::int32_t width, std::int32_t height)
{
    const std::int32_t side = std::min(width - COLUMN_W - COLUMN_GAP, height);
    build_scope(page, side, 80);
    build_legend(page, side);
    build_column(page, side + COLUMN_GAP, height);
    show_radar(s_last);
}

void radar_page_opened()
{
    s_following = true;
}

void show_radar(const radar::Snapshot &snapshot)
{
    if (s_scope == nullptr) {
        return;
    }
    if (&snapshot != &s_last) {
        s_last = snapshot;
    }

    const int range_km = s_last.range_km > 0 ? s_last.range_km : 80;

    s_shown      = 0;
    int rim_used = 0;
    for (int i = 0; i < s_last.count && s_shown < radar::kMaxAircraft; ++i) {
        const radar::Aircraft &aircraft = s_last.list[i];
        if (aircraft.on_ground) {
            continue;
        }
        const float distance_km = aircraft.distance_nm * KM_PER_NM;
        if (distance_km > static_cast<float>(range_km)) {
            // Out past the edge, so it sits on the rim at its bearing instead
            // of vanishing: an empty scope and a quiet sky look the same.
            if (rim_used < RIM_DOTS) {
                draw_rim(s_rim[rim_used++], aircraft);
            }
            continue;
        }
        s_plots[s_shown].aircraft = &s_last.list[i];
        s_plots[s_shown].east_km  = distance_km * std::sin(aircraft.bearing_deg * DEG);
        s_plots[s_shown].north_km = distance_km * std::cos(aircraft.bearing_deg * DEG);
        ++s_shown;
    }
    for (int i = rim_used; i < RIM_DOTS; ++i) {
        lv_obj_set_hidden(s_rim[i], true);
    }
    s_beyond = rim_used;

    std::sort(s_plots, s_plots + s_shown, [](const Plot &a, const Plot &b) {
        return a.aircraft->distance_nm < b.aircraft->distance_nm;
    });

    // Nothing tapped yet, so the nearest aircraft is the subject. It is also the
    // one whose details have already been fetched, so this costs no network.
    if (s_following) {
        if (s_shown > 0) {
            const bool changed = std::strcmp(s_chosen, s_plots[0].aircraft->hex) != 0;
            std::memcpy(s_chosen, s_plots[0].aircraft->hex, sizeof(s_chosen));
            s_chosen[sizeof(s_chosen) - 1] = '\0';
            if (changed && std::strcmp(s_details_hex, s_chosen) != 0) {
                s_picture = Picture::Looking;
                radar::request_details(s_chosen, s_plots[0].aircraft->flight);
            }
        } else {
            s_chosen[0] = '\0';
        }
    }

    for (int i = 0; i < s_shown; ++i) {
        place_blip(s_plots[i], static_cast<float>(range_km));
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
        show_summary(s_last);
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
    // being shown rather than looking as though nothing is coming.
    s_picture = details.photo_url[0] != '\0' ? Picture::Loading : Picture::Missing;
    show_radar(s_last);
}

}  // namespace ui
