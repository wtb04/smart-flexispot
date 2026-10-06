#pragma once

// The design kit: every colour, size, surface, control and motion the panel
// draws with, in one place. Components only ever add the kit's shared styles,
// so switching the look, the accent or the glass restyles every screen at once,
// and a screen that looks different from the others is one that styles itself.
//
// Solid for what stays, glass for what you touch or what floats. Solid things
// are flat fills lifted by one soft shadow. Glass is the dock, the bar's
// capsules, popovers and notices. Tick marks only where a value is read off a
// scale; zone bands only where good, fair and poor are the point.

#include "lvgl.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace ui::kit {

enum class Look : std::uint8_t { Dark, Light };

struct Palette {
    std::uint32_t ground, surface, surface_hi, raised, raised_hi;
    lv_opa_t      fill_opa;  // a flat area, a track or a dial's arc: the ink, faintly, on whatever is under it
    std::uint32_t ink;
    lv_opa_t      ink2_opa, ink3_opa, hair_opa, tick_opa;
    std::uint32_t hair;  // the colour hairlines and the grid are drawn in, at hair_opa
    std::uint32_t accent, on_accent;
    std::uint32_t green, amber, red, lamp, on_lamp, blue;
    std::uint32_t shadow;
    lv_opa_t      shadow_opa, edge_opa;
    std::uint32_t glass;
    lv_opa_t      glass_clear_opa, glass_solid_opa, rim_opa, glass_edge_opa, glass_shadow_opa;
    std::uint32_t scrim;
    lv_opa_t      scrim_opa;
};

namespace space {
inline constexpr std::int32_t xs = 4;
inline constexpr std::int32_t s  = 8;
inline constexpr std::int32_t m  = 12;
inline constexpr std::int32_t l  = 16;  // between tiles, around the screen
inline constexpr std::int32_t xl = 20;  // inside a tile
inline constexpr std::int32_t xxl = 24;
}  // namespace space

namespace radius {
inline constexpr std::int32_t tile   = 26;
inline constexpr std::int32_t inner  = 16;  // a well or a row inside a tile: tile minus its padding, rounded
inline constexpr std::int32_t button = 16;
inline constexpr std::int32_t pop    = 30;
inline constexpr std::int32_t dock   = 34;
inline constexpr std::int32_t round  = LV_RADIUS_CIRCLE;
}  // namespace radius

namespace size {
inline constexpr std::int32_t touch     = 56;  // the smallest thing a finger is asked to hit
inline constexpr std::int32_t button    = 56;
inline constexpr std::int32_t button_lg = 72;
inline constexpr std::int32_t toggle_w  = 92;  // a switch a thumb lands on, not a fingertip
inline constexpr std::int32_t toggle_h  = 52;
inline constexpr std::int32_t bar       = 72;
inline constexpr std::int32_t dock      = 112;
inline constexpr std::int32_t screen_w  = 1280;
inline constexpr std::int32_t screen_h  = 720;
}  // namespace size

// One spring for everything that moves, scaled to each duration: nine tenths
// of the way in two fifths of it, past the end by half a percent, then still.
namespace motion {
inline constexpr std::uint32_t press_ms   = 0;     // the press shows on the first frame after the finger lands
inline constexpr std::uint32_t release_ms = 300;
inline constexpr std::uint32_t pop_ms     = 450;   // to settle, the spring's tail included
inline constexpr std::uint32_t close_ms   = 240;
inline constexpr std::uint32_t move_ms    = 420;   // a needle, a knob, a segment's thumb, a switch
inline constexpr std::uint32_t leave_ms   = 200;   // a notice going, accelerating away
inline constexpr std::uint32_t fade_ms    = 220;
inline constexpr std::int32_t  press_scale = 248;  // of 256
inline constexpr std::int32_t  grab_scale  = 300;  // a knob under the finger
/** A damped spring, barely past its end, normalised to the animation's length. */
int32_t spring(const lv_anim_t *a);
/** The same without passing the end, for going back into something. */
int32_t settle(const lv_anim_t *a);
/**
 * A small shadow close under `obj` while it moves, its own once it stops: a
 * big soft one is drawn again every frame over everything it passes.
 */
void moving(lv_obj_t *obj, bool moving);
}  // namespace motion

enum class Role : std::uint8_t { Label, Small, Body, H3, H2, H1, ReadS, ReadM, ReadL, ReadXL, ReadXXL };
enum class Ink : std::uint8_t { Primary, Secondary, Tertiary, Accent, OnAccent, Green, Amber, Red };

/** Once, after the fonts and before anything is built. */
void init(Look look = Look::Dark);
void set_look(Look look);
/** 0 is the clearest glass, 100 nearly solid. */
void set_glass(int percent);
void set_accent(std::uint32_t colour);
/** For measuring what the panel can afford: blur and shadows can each be left out. */
void set_effects(bool blur, bool shadows);
Look look();
int  glass();
const Palette &colours();
lv_color_t     colour(Ink ink);
/** For widgets that colour their own parts: `cb` runs on every restyle while `owner` lives. */
void           on_restyle(lv_obj_t *owner, void (*cb)(void *ctx), void *ctx);


/** The ground every screen sits on: a warm, lamp-lit dark, or warm paper. */
lv_obj_t *ground(lv_obj_t *parent);

/**
 * The planes a screen is built on, back to front. Content scrolls under the
 * chrome; the chrome casts onto the content; popovers sit over both behind a
 * scrim; notices over everything. A thing's plane decides how it is lit.
 */
enum class Layer : std::uint8_t { Ground, Content, Chrome, Overlay, Alert };
/** Builds the planes on `scr`, the ground among them. Content scrolls, the rest stays put. */
void      screen(lv_obj_t *scr);
lv_obj_t *layer(Layer which);

enum class Lift : std::uint8_t { Low, High };
lv_obj_t *tile(lv_obj_t *parent, Lift lift = Lift::Low);
lv_obj_t *well(lv_obj_t *parent);

// Dock is large glass beside the content: its shadow ends in the gap, as one
// across a tile's edge comes out a hard step in the panel's 16-bit colour.
enum class Glass : std::uint8_t { Small, Large, Clear, Dock };
lv_obj_t *glass(lv_obj_t *parent, Glass kind = Glass::Small);

lv_obj_t *text(lv_obj_t *parent, Role role, const char *value, Ink ink = Ink::Primary);
lv_obj_t *icon(lv_obj_t *parent, const char *glyph, std::int32_t px = 28, bool filled = false,
               Ink ink = Ink::Primary);
void      set_ink(lv_obj_t *label, Ink ink);
void      set_icon(lv_obj_t *label, const char *glyph, bool filled);
lv_obj_t *hairline(lv_obj_t *parent, bool vertical, std::int32_t length);
/** A state at a glance: heating, warm enough, off; good, fair, poor. */
lv_obj_t *dot(lv_obj_t *parent, Ink ink, std::int32_t px = 12);
void      set_dot(lv_obj_t *dot, Ink ink);

/** The bar's status as the panel has always drawn it: the time, Wi-Fi and the phone. */
struct Status {
    lv_obj_t *root = nullptr, *clock = nullptr, *wifi = nullptr, *phone = nullptr;
};
Status status(lv_obj_t *parent);
void   set_status(const Status &s, const char *time, bool wifi, bool phone);

enum class Button : std::uint8_t {
    Raised,  // the ordinary button on a surface
    Accent,  // the one primary action
    Flat,    // on glass, or a bare glyph
    Soft,    // accent-tinted, for a quiet "on"
};
/** A button with an optional icon and text. Checkable buttons fill with the accent when on. */
lv_obj_t *button(lv_obj_t *parent, Button kind, const char *glyph, const char *label,
                 bool round = false, bool checkable = false);
/** A lamp-style toggle tile: a fill in `lamp` when on. */
lv_obj_t *lamp_tile(lv_obj_t *parent, const char *glyph, const char *name, bool on);
void      set_lamp(lv_obj_t *tile, bool on);

/** Faded to 40% and untappable; never LVGL's grey. */
void set_usable(lv_obj_t *obj, bool usable);

lv_obj_t *toggle(lv_obj_t *parent, bool on);

/** Segments in a well; the chosen one sits raised and slides to a tap. */
lv_obj_t *segmented(lv_obj_t *parent, std::initializer_list<const char *> labels, int chosen,
                    std::int32_t width);
int  segmented_chosen(lv_obj_t *seg);
void segmented_choose(lv_obj_t *seg, int index, bool animate);

/** The bar's capsule: a small glass pill that related items share, split by hairlines. */
lv_obj_t *capsule(lv_obj_t *parent);
lv_obj_t *capsule_item(lv_obj_t *capsule, const char *glyph, const char *label, std::int32_t width);
void      capsule_divider(lv_obj_t *capsule);

// --- instruments -----------------------------------------------------------

struct Zone {
    std::int32_t from, to;
    Ink          ink;
    const char  *word = nullptr;  // what a reading in it is: "Good", "Fair", "Poor"
};

struct DialSpec {
    std::int32_t size    = 200;
    std::int32_t min     = 0;
    std::int32_t max     = 100;
    std::int32_t start   = 150;  // degrees clockwise from 3 o'clock, as LVGL counts
    std::int32_t sweep   = 240;
    std::int32_t ticks   = 31;   // total, minor and major
    std::int32_t major   = 5;
    std::int32_t decimals = 0;   // values are in tenths when this is 1
    bool         needle  = true;
    bool         fill    = false;  // an accent arc from the minimum to the target
    bool         target  = false;  // a knob at the target, for a setting
    bool         ends    = false;  // the range in small figures at the two ends
    bool         span    = false;  // a setting by ticks alone: those from the reading up to the target lit, the target marked and read out
    bool         bold    = false;  // heavier ticks, for a dial that is nothing but ticks
    const char  *unit    = nullptr;
    const Zone  *zones   = nullptr;
    int          zone_count = 0;
    Role         read    = Role::ReadM;
};

class Dial {
  public:
    Dial() = default;
    Dial(const Dial &) = delete;
    Dial &operator=(const Dial &) = delete;
    Dial(lv_obj_t *parent, const DialSpec &spec);
    lv_obj_t *obj() const { return m_root; }
    /** Moves the needle and the readout there on a spring. */
    void set_value(std::int32_t value, bool animate = true);
    void set_target(std::int32_t value, bool animate = true);
    void set_lit(int ticks);  // the first n ticks in the accent, for a timer
    void set_caption(const char *caption);  // the small line under the readout
    std::int32_t value() const { return m_value; }

  private:
    static void on_value(void *var, int32_t v);
    static void on_target(void *var, int32_t v);
    void place_needle(std::int32_t value);
    void place_line(lv_obj_t *line, lv_point_precise_t *points, std::int32_t value, float r0, float r1);
    void light_span();
    void place_target(std::int32_t value);
    void show_value(std::int32_t value);
    void light_zone(std::int32_t value);
    void restyle();
    void stale_face();
    void make_face();
    float angle_of(std::int32_t value) const;

    DialSpec  m_spec{};
    lv_obj_t *m_root   = nullptr;
    lv_obj_t *m_scale  = nullptr;
    lv_obj_t *m_needle = nullptr;
    lv_obj_t *m_arc    = nullptr;
    lv_obj_t *m_knob   = nullptr;
    lv_obj_t *m_read   = nullptr;
    lv_obj_t *m_unit   = nullptr;
    lv_obj_t *m_face   = nullptr;  // the scale as a picture, shown in its place
    lv_draw_buf_t *m_face_buf   = nullptr;
    lv_timer_t    *m_face_timer = nullptr;
    lv_obj_t *m_mark   = nullptr;  // the target, on a span dial
    lv_point_precise_t m_points[2]{}, m_mark_points[2]{};
    std::int32_t m_span_lo = 0, m_span_hi = -1;
    lv_scale_section_t *m_zones[6]{};
    lv_style_t          m_zone_styles[6]{};
    lv_scale_section_t *m_lit_section = nullptr;
    lv_style_t          m_lit_style{};
    char                m_ends[2][12]{};
    const char         *m_src[72]{};
    std::int32_t m_value = 0, m_target = 0;
    std::int32_t m_shown = INT32_MIN, m_lit = -1, m_zone = -1;
    std::int32_t m_r_tick = 0, m_major_len = 0, m_r_arc = 0;
};

struct RailSpec {
    std::int32_t length   = 300;
    bool         vertical = false;
    std::int32_t min = 0, max = 100;
    std::int32_t ticks    = 0;  // 0: a plain track, as sliders and progress are
    std::int32_t major    = 5;
    bool         draggable = false;
    bool         fill     = true;
    bool         quiet    = false;  // a thin line, its knob only under the finger: a track's progress
    const std::int32_t *notches = nullptr;  // marks for presets
    int          notch_count = 0;
};

class Rail {
  public:
    Rail() = default;
    Rail(const Rail &) = delete;
    Rail &operator=(const Rail &) = delete;
    Rail(lv_obj_t *parent, const RailSpec &spec);
    lv_obj_t *obj() const { return m_root; }
    void set_value(std::int32_t value, bool animate = true);
    std::int32_t value() const { return m_value; }

  private:
    static void on_value(void *var, int32_t v);
    static void on_press(lv_event_t *e);
    void place(std::int32_t value);
    void restyle();

    RailSpec  m_spec{};
    lv_obj_t *m_root = nullptr, *m_track = nullptr, *m_fill = nullptr, *m_knob = nullptr;
    std::int32_t m_value = 0, m_shown = 0;
};

// --- popovers -------------------------------------------------------------

/** Centre: in the middle of the screen whatever opened it, for a sheet. */
enum class From : std::uint8_t { Below, Beside, Centre };

/**
 * How a popover's glass is made. Live blurs what is behind it on every frame
 * it changes; Frozen takes one picture of the screen as it opens, blurs that
 * once and lays the card over it, so growing only moves an outline.
 */
enum class PopGlass : std::uint8_t { Live, Frozen };
void     set_pop_glass(PopGlass how);
PopGlass pop_glass();
/** Grow: the outline opens out of the source. Rise: the card fades in at its place, coming up a little. */
enum class PopMotion : std::uint8_t { Grow, Rise };
void      set_pop_motion(PopMotion how);
PopMotion pop_motion();
/** The last frozen picture, for looking at from a development build. */
const lv_draw_buf_t *frozen_picture();
/** How long the last frozen popover took to take its picture and blur it. */
const char *freeze_report();

/**
 * A glass card that grows out of `source` on a spring, over a scrim that closes
 * it on a tap. Returns the card to fill; its size is `w` by `h`.
 */
lv_obj_t *popover(lv_obj_t *source, std::int32_t w, std::int32_t h, From from = From::Below,
                  bool blur_scrim = true);
void      close_popover(bool animate = true);
/** Something that opens a popover: its backdrop starts being made as a finger lands on it. */
void      popover_source(lv_obj_t *obj);
/** What a finger landing on a popover source does: start on the backdrop. */
void      prepare_popover();
bool      popover_open();

/** A slab that drops from under the bar with a notice, and leaves on its own. */
lv_obj_t *notice(const char *source, const char *glyph, const char *title, const char *message,
                 std::uint32_t timeout_ms);

namespace perf {
/** Microseconds from some fixed point; the platform's best clock. Without one, LVGL's ms tick. */
using Clock = std::uint64_t (*)();
void start(Clock clock = nullptr);
struct Stats {
    std::uint32_t frames   = 0;
    float         avg_ms   = 0;
    float         worst_ms = 0;
    std::uint32_t span_ms  = 0;  // how long the measuring ran
};
/** Frames drawn in the second before last. */
Stats last_second();
void  begin();
Stats end();
/** Each frame of the last measuring, in ms, up to 48 of them. */
int   frames(const std::uint16_t **ms);
/** Goes up with every frame drawn, so a picture of the screen can tell whether it is still current. */
std::uint32_t generation();
std::uint64_t last_frame_us();
std::uint64_t now_us();
}  // namespace perf

// --- pictures -------------------------------------------------------------

/** A picture as it arrives: RGB565, `stride` bytes a row (0 for w * 2). Read only during the call it is given to. */
struct Picture {
    const void  *pixels = nullptr;
    std::int32_t w = 0, h = 0, stride = 0;
};

enum class Shape : std::uint8_t { Rounded, Round };

/**
 * A picture in a rounded rectangle or a circle: album art, a video's still, a
 * channel's or a plane's picture. It is scaled to its size and its corners cut
 * once, as it arrives, so drawing it is a plain copy; a new one fades in over
 * the old. Without one it is an empty well with a glyph.
 */
class Cover {
  public:
    Cover(const Cover &) = delete;
    Cover &operator=(const Cover &) = delete;
    /** `radius` below 0 picks one for the size; `glyph` is what the empty well shows, a note by default. */
    Cover(lv_obj_t *parent, std::int32_t w, std::int32_t h, Shape shape = Shape::Rounded, std::int32_t radius = -1,
          const char *glyph = nullptr);
    lv_obj_t *obj() const { return m_root; }
    /** False when there is no memory for it, and the cover is left as it was. */
    bool show(const Picture &picture, bool animate = true);
    /** Back to the empty well. */
    void clear(bool animate = true);
    bool        has_picture() const { return m_cur.main != nullptr; }
    std::size_t bytes() const;

    struct Made {
        lv_draw_buf_t *main = nullptr;  // RGB565, or RGB565A8 when it is all corner
        lv_draw_buf_t *corner[4]{};     // RGB565A8, radius square: top left, top right, bottom left, bottom right
    };

  private:
    static void on_draw(lv_event_t *e);
    static void on_mix(void *var, int32_t v);
    void fade(bool animate);
    void settle();

    lv_obj_t    *m_root = nullptr, *m_well = nullptr, *m_pic = nullptr;
    Made         m_cur{}, m_prev{};
    std::int32_t m_w = 0, m_h = 0, m_r = 0;
    std::int32_t m_mix = 255;
};

/**
 * What the music and the cinema views lay behind everything: their picture
 * enlarged to fill, blurred, dithered, and darkened in the dark look or
 * lightened in the light one so their text reads on it. Made off the LVGL task
 * and laid down a band of rows a frame; opaque, so nothing under it is drawn.
 */
class Backdrop {
  public:
    Backdrop(const Backdrop &) = delete;
    Backdrop &operator=(const Backdrop &) = delete;
    Backdrop(lv_obj_t *parent, std::int32_t w, std::int32_t h, std::int32_t radius = 0);
    lv_obj_t *obj() const { return m_root; }
    /** False when there is no memory for it, and the backdrop is left as it was. */
    bool show(const Picture &picture);
    /** The plain ground again, and its memory given back. */
    void clear();
    std::size_t bytes() const;

    struct Job;

  private:
    static void on_draw(lv_event_t *e);
    static void on_cover(lv_event_t *e);
    static void on_tick(lv_timer_t *t);
    bool start();
    void stop();
    void restyle();

    lv_obj_t      *m_root = nullptr;
    Cover::Made    m_made{};
    std::uint16_t *m_small = nullptr;  // the blurred picture, untinted, at an eighth, three planes
    std::int32_t   m_w = 0, m_h = 0, m_r = 0, m_sw = 0, m_sh = 0;
    std::int32_t   m_shown = 0;  // rows that hold a picture, until it is whole
    std::int32_t   m_pass  = 0;  // rows laid down by the picture being made
    bool           m_whole = false;
    Job           *m_job = nullptr;
    lv_timer_t    *m_timer = nullptr;
};

/** What the last cover and backdrop held and took to make. */
const char *pictures_report();

// --- pages ----------------------------------------------------------------

/** Where a page's contents lie: under the bar, right of the dock, the screen's margin round the rest. */
namespace inset {
inline constexpr std::int32_t top    = 16 + size::bar + space::l;
inline constexpr std::int32_t left   = 14 + size::dock + space::l;
inline constexpr std::int32_t right  = space::l;
inline constexpr std::int32_t bottom = space::l;
inline constexpr std::int32_t width  = size::screen_w - left - right;
inline constexpr std::int32_t height = size::screen_h - top - bottom;
}  // namespace inset

/**
 * A page: the whole screen on the Content layer, what is on it inset, so it
 * scrolls up under the bar. Up and down only, and never past its ends: a page
 * pulled past them is drawn again whole on every frame. Hidden until shown.
 */
lv_obj_t *page();

/**
 * How one page gives way to the next. A whole screen drawn in one frame is the
 * slowest frame the panel has: all but Slide and Fade draw the new page once,
 * Wipe and Spread a band a frame, Wipe from the dock side and Spread from the
 * chosen item out, Stagger the ground first and then a tile a frame. Slide and
 * Fade draw all of it every frame, to compare.
 */
enum class Change : std::uint8_t { Instant, Wipe, Spread, Stagger, Slide, Fade };
void   set_change(Change how);
Change change();

/** A page's item in the dock: a glyph the dock's mark slides behind when its page is chosen. */
lv_obj_t *dock_item(lv_obj_t *dock, const char *glyph);
/** A page and the dock item that chooses it; `build` fills it in a pause ahead of its first visit. */
int       add_page(lv_obj_t *item, void (*build)(lv_obj_t *page));
void      show_page(int index, bool animate = true);
int       shown_page();
lv_obj_t *page_at(int index);
/** No page shows, for a screen with other content of its own; showing one brings them back. */
void      hide_pages();
/** Told of each page as it comes to show, before anything of it is drawn. */
void      on_page_shown(void (*cb)(int index));
/** The last page change: its frames, and how long it took to start. */
const char *change_report();
/** The edge under the bar follows how far `scroller` is scrolled, from now on and as it is now. */
void      edge_follows(lv_obj_t *scroller);

/** A widget's size in the grid's cells: one, two across, two down, two by two. */
enum class Span : std::uint8_t { One, Wide, Tall, Big };
/** Cells four across and three to the page's height; widgets take the first place they fit, in the order made. */
lv_obj_t *widget_grid(lv_obj_t *page, int columns = 4);
/** A widget: a tile with its caption, for showing only. Returns its body, to fill. */
lv_obj_t *widget(lv_obj_t *grid, Span span, const char *caption);
/** A tap on the widget whose body this is opens a page, as the radar's opens the radar. */
void      widget_opens(lv_obj_t *body, int page);

// --- lists and readings ----------------------------------------------------
//
// Rows draw no fill of their own until pressed and their hairlines are drawn
// with them, not made as objects; whatever scrolls sits inside something
// opaque that covers it, so a frame of scrolling is that one fill and the
// rows in sight. See lab_lists.cpp for what makes a long list cheap.

/** How wide the widest of `texts` is set in `role`: the width for a label that shows any of them. */
std::int32_t widest(Role role, std::initializer_list<const char *> texts);

/** A small caption over a group, or a day's name in a list, with `aside` at its right. */
lv_obj_t *section(lv_obj_t *parent, const char *title, const char *aside = nullptr);

/** Rows in one tile, split by hairlines; the first and last row's press reaches into its corners. */
lv_obj_t *group(lv_obj_t *parent, std::int32_t width);

/** A tile `w` by `h` with a column that scrolls inside it. Returns the column, for rows and sections. */
lv_obj_t *list(lv_obj_t *parent, std::int32_t w, std::int32_t h);

enum class Trail : std::uint8_t { None, Value, Chevron, Toggle, Badge };

struct RowSpec {
    const char   *glyph  = nullptr;
    Ink           tint   = Ink::Accent;  // the glyph and the square it sits in
    bool          bare   = false;        // the glyph without its square
    const char   *lead   = nullptr;      // a time or a figure before the rest, `lead_w` wide (0: as it is)
    std::int32_t  lead_w = 0;
    std::uint32_t mark   = 0;            // a bar this colour before the title, as a calendar's; 0 for none
    const char   *title  = "";
    const char   *detail = nullptr;      // a second line under the title
    Trail         trail  = Trail::None;
    const char   *value  = nullptr;      // Value, a Chevron's text before it, a Badge's text
    std::int32_t  value_w = 0;           // for a value that changes, the widest it shows (0: as it is)
    Ink           ink    = Ink::Accent;  // a Badge's, or the state dot's
    bool          dot    = false;        // a state dot before the value, or in the badge
    bool          on     = false;        // Toggle
    bool          action = false;        // the title in the accent: a thing to do
    bool          pressable = false;     // without a chevron, as an event that opens when tapped
};

/**
 * A row in a group or a list. One with a chevron, a toggle or an action is
 * pressable and sends LV_EVENT_CLICKED; a toggle row's tap flips the toggle,
 * which sends LV_EVENT_VALUE_CHANGED. The rest are not pressable. Under
 * something that scrolls the press shows a moment late, so a swipe through
 * the rows does not light them all.
 */
lv_obj_t *row(lv_obj_t *parent, const RowSpec &spec);
void      set_row_value(lv_obj_t *row, const char *value);
/** The value and its dot's ink, or the badge's text and ink. */
void      set_row_state(lv_obj_t *row, const char *value, Ink ink);
void      set_row_detail(lv_obj_t *row, const char *detail);
/** Its value, toggle or badge. */
lv_obj_t *row_trail(lv_obj_t *row);

/**
 * A small pill: Accent, Green, Amber and Red tint it, Primary to Tertiary are
 * neutral, OnAccent fills it with the accent. `width` 0 is as wide as `text`;
 * one whose text changes is given the widest it shows. `dot`: a state dot
 * before the text, in its ink.
 */
lv_obj_t *badge(lv_obj_t *parent, const char *text, Ink ink = Ink::Secondary, const char *glyph = nullptr,
                std::int32_t width = 0, bool dot = false);
void      set_badge(lv_obj_t *badge, const char *text, Ink ink);

/** A glyph and a line in Tertiary, where a feed is missing or failing. */
lv_obj_t *empty(lv_obj_t *parent, const char *glyph, const char *text);

struct ReadoutSpec {
    Role         role     = Role::ReadL;
    const char  *unit     = nullptr;
    const char  *caption  = nullptr;
    std::int32_t min      = 0, max = 999;  // its width is that of the widest of these
    std::int32_t decimals = 0;             // values are in tenths when this is 1
};

/** A big figure in a box that never changes size, so nothing round it moves when it does. */
class Readout {
  public:
    Readout() = default;
    Readout(const Readout &) = delete;
    Readout &operator=(const Readout &) = delete;
    Readout(lv_obj_t *parent, const ReadoutSpec &spec);
    ~Readout();
    lv_obj_t *obj() const { return m_root; }
    /** Counts there on the spring when `animate`. */
    void set_value(std::int32_t value, bool animate = false);
    /** No reading: a dash, never a made-up figure. */
    void clear();
    std::int32_t value() const { return m_value; }

  private:
    static void on_count(void *var, int32_t v);
    void show(std::int32_t value);

    ReadoutSpec  m_spec{};
    lv_obj_t    *m_root = nullptr, *m_read = nullptr;
    std::int32_t m_value = 0, m_shown = INT32_MIN;
};

struct StepperSpec {
    std::int32_t min = 0, max = 100, step = 1, value = 0;
    std::int32_t decimals = 0;
    const char  *unit = nullptr;
};

/** Minus, the value, plus: holding one repeats, faster the longer it is held. Sends LV_EVENT_VALUE_CHANGED. */
class Stepper {
  public:
    Stepper() = default;
    Stepper(const Stepper &) = delete;
    Stepper &operator=(const Stepper &) = delete;
    Stepper(lv_obj_t *parent, const StepperSpec &spec);
    ~Stepper();
    lv_obj_t *obj() const { return m_root; }
    void set_value(std::int32_t value);
    std::int32_t value() const { return m_value; }

  private:
    static void on_press(lv_event_t *e);
    static void on_repeat(lv_timer_t *t);
    void step(int dir, int times);
    void show();
    void stop();

    StepperSpec  m_spec{};
    lv_obj_t    *m_root = nullptr, *m_less = nullptr, *m_more = nullptr, *m_read = nullptr;
    lv_timer_t  *m_timer = nullptr;
    std::int32_t m_value = 0;
    int          m_dir = 0, m_repeats = 0;
};

/**
 * A big glass card for long content, opened out of `source` as a popover and
 * centred on the screen, with a title and a close button over a body that
 * scrolls. Returns the body: fill it with rows, sheet_row or text.
 */
lv_obj_t *sheet(lv_obj_t *source, const char *title, std::int32_t w = 760, std::int32_t h = 600);
/** A label and its value, a line of a sheet. Returns the value's label. */
lv_obj_t *sheet_row(lv_obj_t *body, const char *label, const char *value);

// --- readings --------------------------------------------------------------

/** A thin bar of zones with a dot at the reading: the zone it is in bright, the rest faint. */
class Scale {
  public:
    Scale() = default;
    Scale(const Scale &) = delete;
    Scale &operator=(const Scale &) = delete;
    /** `width` 0: as wide as its parent's content, laid out again as that changes. */
    Scale(lv_obj_t *parent, std::int32_t width, std::int32_t min, std::int32_t max, const Zone *zones, int count);
    ~Scale();
    lv_obj_t *obj() const { return m_root; }
    void      set_value(std::int32_t value, bool animate = true);

  private:
    static void on_move(void *var, int32_t v);
    void        place(std::int32_t value);
    void        lay_out();
    void        restyle();
    int         zone_of(std::int32_t value) const;

    lv_obj_t    *m_root = nullptr, *m_dot = nullptr;
    lv_obj_t    *m_bands[6]{};
    const Zone  *m_zones = nullptr;
    int          m_count = 0, m_lit = -2;
    std::int32_t m_min = 0, m_max = 100, m_w = 0, m_value = 0, m_shown = INT32_MIN;
};

struct ReadingSpec {
    const char  *caption = nullptr;  // in sentence case: "CO₂", "Humidity"
    const char  *unit    = nullptr;
    std::int32_t min = 0, max = 100;  // the scale's ends; the figure's width is that of the widest of these
    const Zone  *zones = nullptr;
    int          zone_count = 0;
    const char  *note = nullptr;  // a quiet line under the scale: "Good up to 800"
};

/** What a reading is, at a glance: its name, a big figure, one word in its zone's ink, and the scale it sits on. */
class Reading {
  public:
    Reading(const Reading &) = delete;
    Reading &operator=(const Reading &) = delete;
    /** Fills `parent`, a tile, from top to bottom. */
    Reading(lv_obj_t *parent, const ReadingSpec &spec);
    void set_value(std::int32_t value, bool animate = true);
    /** No reading: a dash, no word, no dot. */
    void clear();

  private:
    void        show(std::int32_t value);

    ReadingSpec m_spec{};
    lv_obj_t   *m_figure = nullptr, *m_unit = nullptr, *m_word = nullptr;
    std::int32_t m_shown = INT32_MIN;
    Scale      *m_scale = nullptr;
    int         m_zone = -2;
};

}  // namespace ui::kit
