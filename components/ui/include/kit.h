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

enum class Glass : std::uint8_t { Small, Large, Clear };
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

enum class From : std::uint8_t { Below, Beside };

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

}  // namespace ui::kit
