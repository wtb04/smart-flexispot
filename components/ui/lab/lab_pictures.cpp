#include "kit.h"
#include "kit_fonts.h"
#include "lvgl_private.h"  // to drop a picture from LVGL's cache before it goes

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

// The lab's sheet of pictures: covers in two sizes and both shapes, the empty
// well while one is awaited, one on glass, and a music view's layout over the
// backdrop its cover makes. The pictures are drawn here, as the lab has no
// feed of its own; on the panel they come from the covers' fetcher.
namespace ui::lab {
namespace {

using namespace ui::kit;
namespace ic = ui::kit::glyph;

constexpr std::int32_t SOURCE = 240;  // a fetched cover's side, between the card's and the music view's
constexpr std::int32_t STILL_W = 320, STILL_H = 180;
constexpr std::int32_t PANEL_W = 640, PANEL_H = 360;
constexpr std::int32_t MUSIC_COVER = 200;
constexpr std::int32_t MUSIC_X = 32, MUSIC_Y = (PANEL_H - MUSIC_COVER) / 2;

struct Rgb {
    float r, g, b;
};

Rgb rgb(std::uint32_t c)
{
    return {static_cast<float>((c >> 16) & 0xff) / 255.0f, static_cast<float>((c >> 8) & 0xff) / 255.0f,
            static_cast<float>(c & 0xff) / 255.0f};
}

Rgb mix(Rgb a, Rgb b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

std::uint16_t pack(Rgb c)
{
    const auto q = [](float v, int top) { return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * static_cast<float>(top) + 0.5f); };
    return static_cast<std::uint16_t>(q(c.r, 31) << 11 | q(c.g, 63) << 5 | q(c.b, 31));
}

float edge(float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); }  // a pixel's share inside a shape, d its signed distance in pixels

// A dusk over a grid, as a synthwave sleeve has it.
Rgb dusk(float x, float y, float px)
{
    const float horizon = 0.64f;
    if (y < horizon) {
        Rgb c = mix(rgb(0x1c0b40), rgb(0xff7a45), std::pow(y / horizon, 1.6f));
        const float dx = x - 0.5f, dy = y - 0.5f, d = std::sqrt(dx * dx + dy * dy);
        const bool  gap = y > 0.5f && std::fmod((y - 0.5f) * 40.0f, 1.0f) < (y - 0.5f) * 6.0f;
        if (!gap) {
            c = mix(c, mix(rgb(0xffe066), rgb(0xff4f8b), (y - 0.26f) / 0.38f), edge((d - 0.24f) / px));
        }
        return c;
    }
    const float t = (y - horizon) / (1.0f - horizon);
    Rgb         c = mix(rgb(0x2a0f45), rgb(0x0e0618), t);
    const float row = std::fmod(std::sqrt(t) * 7.0f, 1.0f);
    const float col = std::fmod((x - 0.5f) / (0.08f + t) * 2.0f + 50.0f, 1.0f);
    const float line = std::max(edge((std::min(row, 1.0f - row) - 0.04f) / (px * 8.0f)),
                                edge((std::min(col, 1.0f - col) - 0.03f) / (px * 6.0f)));
    return mix(c, rgb(0xff3fa4), line * (0.4f + 0.6f * t));
}

// Soft circles on a sea's green and blue.
Rgb tide(float x, float y, float px)
{
    Rgb c = mix(rgb(0x0b2f52), rgb(0x2fb7a2), (x + y) / 2.0f);
    struct C { float x, y, r; std::uint32_t colour; float a; };
    for (const C &k : {C{0.30f, 0.34f, 0.26f, 0xffffff, 0.16f}, C{0.66f, 0.58f, 0.30f, 0xffffff, 0.12f},
                       C{0.52f, 0.30f, 0.12f, 0xf4f1d0, 0.85f}, C{0.38f, 0.74f, 0.18f, 0x071c33, 0.7f}}) {
        const float d = std::sqrt((x - k.x) * (x - k.x) + (y - k.y) * (y - k.y)) - k.r;
        c = mix(c, rgb(k.colour), k.a * edge(d / px));
    }
    return c;
}

// Flat shapes on paper.
Rgb bauhaus(float x, float y, float px)
{
    Rgb c = rgb(0xf1e6d0);
    const float d = std::sqrt((x - 0.38f) * (x - 0.38f) + (y - 0.42f) * (y - 0.42f)) - 0.25f;
    c = mix(c, rgb(0xd8452b), edge(d / px));
    const float q = std::sqrt((x - 1.0f) * (x - 1.0f) + (y - 1.0f) * (y - 1.0f)) - 0.42f;
    c = mix(c, rgb(0x1f4e9c), edge(q / px));
    if (x > 0.56f && x < 0.9f && y > 0.12f && y < 0.32f) {
        c = rgb(0xf2b631);
    }
    if (x > 0.08f && x < 0.64f && y > 0.72f && y < 0.77f) {
        c = rgb(0x1b1b1b);
    }
    return c;
}

// Hills under a low sun, for a video's still.
Rgb field(float x, float y, float px)
{
    Rgb c = mix(rgb(0x79b2e6), rgb(0xf6dcb0), y / 0.7f);
    const float sun = std::sqrt((x - 0.72f) * (x - 0.72f) + (y - 0.36f) * (y - 0.36f) * 3.16f) - 0.07f;
    c = mix(c, rgb(0xfff2c8), edge(sun / px));
    struct H { float base, amp, freq, phase; std::uint32_t colour; };
    for (const H &h : {H{0.62f, 0.06f, 7.0f, 0.4f, 0x7da35c}, H{0.72f, 0.07f, 5.0f, 2.1f, 0x4f7d40},
                       H{0.84f, 0.05f, 9.0f, 4.0f, 0x2c5230}}) {
        const float top = h.base - h.amp * std::sin(x * h.freq + h.phase);
        c = mix(c, rgb(h.colour), edge((top - y) / (px * 1.8f)));
    }
    return c;
}

using Painter = Rgb (*)(float, float, float);
constexpr Painter PICTURES[] = {dusk, tide, bauhaus};
struct Track {
    const char *title, *artist, *length;
};
constexpr Track TRACKS[] = {{"Midnight City", "M83", "4:03"}, {"Teardrop", "Massive Attack", "5:29"},
                            {"Paper Planes", "M.I.A.", "3:24"}};

// Made when wanted and handed straight on, as a fetched one is.
struct Source {
    std::uint16_t *pixels = nullptr;
    Picture        picture{};
    Source(Painter paint, std::int32_t w, std::int32_t h)
    {
        pixels = static_cast<std::uint16_t *>(lv_malloc(sizeof(std::uint16_t) * w * h));
        if (pixels == nullptr) {
            return;
        }
        const float side = static_cast<float>(std::min(w, h));
        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                pixels[y * w + x] = pack(paint((static_cast<float>(x) + 0.5f) / side - (static_cast<float>(w) / side - 1.0f) / 2.0f,
                                               (static_cast<float>(y) + 0.5f) / side, 1.0f / side));
            }
        }
        picture = {pixels, w, h, 0};
    }
    ~Source() { lv_free(pixels); }
    Source(const Source &) = delete;
    Source &operator=(const Source &) = delete;
};

Cover    *s_cycled[5]{};
Cover    *s_music = nullptr;
Backdrop *s_backdrop = nullptr;
lv_obj_t *s_title = nullptr, *s_artist = nullptr, *s_length = nullptr, *s_stats = nullptr, *s_empty = nullptr;
int       s_picture = 0, s_track = 0;
bool      s_shown = false;

void set_text(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

void show_stats()
{
    if (s_stats == nullptr) {
        return;
    }
    std::size_t covers = 0;
    for (const Cover *c : s_cycled) {
        covers += c != nullptr ? c->bytes() : 0;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "Covers here %.0f KB, the music one %.0f KB, its backdrop %.0f KB",
                  static_cast<double>(covers) / 1024.0, static_cast<double>(s_music->bytes()) / 1024.0,
                  static_cast<double>(s_backdrop->bytes()) / 1024.0);
    set_text(s_stats, buf);
}

void next_cover()
{
    s_picture = (s_picture + 1) % static_cast<int>(std::size(PICTURES));
    if (s_empty != nullptr && lv_obj_has_state(s_empty, LV_STATE_CHECKED)) {
        for (Cover *c : s_cycled) {
            c->clear(true);
        }
    } else {
        const Source src(PICTURES[s_picture], SOURCE, SOURCE);
        for (Cover *c : s_cycled) {
            c->show(src.picture, true);
        }
    }
    show_stats();
}

void play_track(bool animate)
{
    const Track &t = TRACKS[s_track];
    set_text(s_title, t.title);
    set_text(s_artist, t.artist);
    set_text(s_length, t.length);
    const Source src(PICTURES[s_track], SOURCE, SOURCE);
    s_music->show(src.picture, animate);
    if (s_shown) {
        s_backdrop->show(src.picture);
    }
    show_stats();
}

void next_track()
{
    s_track = (s_track + 1) % static_cast<int>(std::size(TRACKS));
    play_track(true);
}

template <typename T> void own(lv_obj_t *owner, T *thing)
{
    lv_obj_add_event_cb(owner, [](lv_event_t *e) { delete static_cast<T *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, thing);
}

lv_obj_t *row(lv_obj_t *parent, std::int32_t gap)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(obj, gap, 0);
    lv_obj_set_size(obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return obj;
}

Cover *cover(lv_obj_t *parent, std::int32_t w, std::int32_t h, Shape shape, const char *glyph = nullptr)
{
    auto *c = new Cover(parent, w, h, shape, -1, glyph);
    own(c->obj(), c);
    return c;
}

void build_covers(lv_obj_t *sheet, std::int32_t height)
{
    lv_obj_t *t = tile(sheet, Lift::Low);
    lv_obj_set_size(t, 440, height);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, space::m, 0);
    text(t, Role::Label, "COVERS, CORNERS CUT AS THEY ARRIVE", Ink::Secondary);
    lv_obj_t *big = row(t, space::m);
    s_cycled[0] = cover(big, 148, 148, Shape::Rounded);
    s_cycled[1] = cover(big, 148, 148, Shape::Round);
    lv_obj_t *small = lv_obj_create(big);
    lv_obj_remove_style_all(small);
    lv_obj_set_size(small, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(small, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(small, 20, 0);
    s_cycled[2] = cover(small, 64, 64, Shape::Rounded);
    s_cycled[3] = cover(small, 64, 64, Shape::Round);

    text(t, Role::Label, "A STILL, AND WAITING FOR ONE", Ink::Secondary);
    lv_obj_t *waiting = row(t, space::m);
    Cover    *still   = cover(waiting, 114, 64, Shape::Rounded, ic::aspect_ratio);
    const Source frame(field, STILL_W, STILL_H);
    still->show(frame.picture, false);
    cover(waiting, 114, 64, Shape::Rounded, ic::aspect_ratio);
    cover(waiting, 64, 64, Shape::Rounded);
    cover(waiting, 64, 64, Shape::Round, ic::flight);

    text(t, Role::Label, "ON GLASS", Ink::Secondary);
    lv_obj_t *cap = capsule(t);
    lv_obj_set_width(cap, 300);
    lv_obj_set_style_pad_column(cap, space::m, 0);
    s_cycled[4] = cover(cap, 52, 52, Shape::Rounded);
    lv_obj_t *col = lv_obj_create(cap);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    text(col, Role::H3, "Over what passes");
    text(col, Role::Small, "Its corners blend", Ink::Secondary);

    lv_obj_t *buttons = row(t, space::m);
    lv_obj_t *next    = button(buttons, Button::Raised, ic::skip_next, "Next picture");
    lv_obj_add_event_cb(next, [](lv_event_t *) { next_cover(); }, LV_EVENT_CLICKED, nullptr);
    s_empty = button(buttons, Button::Raised, nullptr, "None", false, true);
    lv_obj_add_event_cb(
        s_empty,
        [](lv_event_t *) {
            if (lv_obj_has_state(s_empty, LV_STATE_CHECKED)) {
                for (Cover *c : s_cycled) {
                    c->clear(true);
                }
            } else {
                const Source src(PICTURES[s_picture], SOURCE, SOURCE);
                for (Cover *c : s_cycled) {
                    c->show(src.picture, true);
                }
            }
            show_stats();
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
}

void build_music(lv_obj_t *sheet, std::int32_t height)
{
    lv_obj_t *col = lv_obj_create(sheet);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(col, PANEL_W, height);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, space::m, 0);

    s_backdrop = new Backdrop(col, PANEL_W, PANEL_H, radius::tile);
    own(s_backdrop->obj(), s_backdrop);
    lv_obj_t *panel = s_backdrop->obj();
    s_music = cover(panel, MUSIC_COVER, MUSIC_COVER, Shape::Rounded);
    lv_obj_set_pos(s_music->obj(), MUSIC_X, MUSIC_Y);
    lv_obj_add_flag(s_music->obj(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_music->obj(), [](lv_event_t *) { next_track(); }, LV_EVENT_CLICKED, nullptr);

    const std::int32_t x = MUSIC_X + MUSIC_COVER + 32, w = PANEL_W - x - 32;
    lv_obj_t *words = lv_obj_create(panel);
    lv_obj_remove_style_all(words);
    lv_obj_remove_flag(words, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(words, w, LV_SIZE_CONTENT);
    lv_obj_align(words, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(words, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(words, 4, 0);
    text(words, Role::Small, "Office speaker", Ink::Secondary);
    s_title = text(words, Role::H1, "");
    s_artist = text(words, Role::Body, "", Ink::Secondary);
    for (lv_obj_t *l : {s_title, s_artist}) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_t *gap = lv_obj_create(words);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, 1, space::m);
    RailSpec rs;
    rs.length = w, rs.min = 0, rs.max = 243, rs.draggable = true, rs.quiet = true;
    auto *progress = new Rail(words, rs);
    own(progress->obj(), progress);
    progress->set_value(102, false);
    lv_obj_t *times = lv_obj_create(words);
    lv_obj_remove_style_all(times);
    lv_obj_set_size(times, w, LV_SIZE_CONTENT);
    lv_obj_align(text(times, Role::Small, "1:42", Ink::Secondary), LV_ALIGN_LEFT_MID, 0, 0);
    s_length = text(times, Role::Small, "", Ink::Secondary);
    lv_obj_align(s_length, LV_ALIGN_RIGHT_MID, 0, 0);

    // plain white marks, the middle one the largest, and nothing behind them until pressed
    lv_obj_t *transport = row(words, space::xxl);
    lv_obj_set_width(transport, w);
    lv_obj_set_style_margin_top(transport, space::l, 0);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *prev = button(transport, Button::Flat, ic::skip_previous, nullptr, true);
    lv_obj_set_size(prev, 72, 72);
    lv_obj_add_event_cb(prev, [](lv_event_t *) {
        s_track = (s_track + static_cast<int>(std::size(TRACKS)) - 1) % static_cast<int>(std::size(TRACKS));
        play_track(true);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *play = button(transport, Button::Flat, ic::pause, nullptr, true);
    lv_obj_set_size(play, 88, 88);
    lv_obj_add_event_cb(
        play,
        [](lv_event_t *e) {
            lv_obj_t *glyph = lv_obj_get_child(lv_event_get_current_target_obj(e), 0);
            set_icon(glyph, std::strcmp(lv_label_get_text(glyph), ic::pause) == 0 ? ic::play_arrow : ic::pause, true);
        },
        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *next = button(transport, Button::Flat, ic::skip_next, nullptr, true);
    lv_obj_set_size(next, 72, 72);
    for (lv_obj_t *b : {prev, play, next}) {
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), fonts::icon(34, true), 0);
    }
    lv_obj_add_event_cb(next, [](lv_event_t *) { next_track(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *note = text(col, Role::Small, "A backdrop made from the cover, blurred off the LVGL task and laid down in bands. "
                                            "Tap the cover or skip to change it.", Ink::Tertiary);
    lv_obj_set_width(note, PANEL_W);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    s_stats = text(col, Role::Small, "", Ink::Secondary);
    lv_obj_set_width(s_stats, PANEL_W);
    lv_label_set_long_mode(s_stats, LV_LABEL_LONG_MODE_CLIP);
}

// --- the bench's ways of rounding a picture's corners, to set beside the cover's ---

lv_obj_t      *s_variant = nullptr;
lv_draw_buf_t *s_variant_buf = nullptr;

void drop_variant()
{
    if (s_variant != nullptr) {
        lv_obj_delete(s_variant);
        s_variant = nullptr;
    }
    if (s_variant_buf != nullptr) {
        lv_image_cache_drop(s_variant_buf);
        lv_draw_buf_destroy(s_variant_buf);
        s_variant_buf = nullptr;
    }
    if (s_music != nullptr) {
        lv_obj_remove_flag(s_music->obj(), LV_OBJ_FLAG_HIDDEN);
    }
}

}  // namespace

void build_pictures(lv_obj_t *sheet)
{
    const std::int32_t h = lv_obj_get_style_height(sheet, LV_PART_MAIN);
    build_covers(sheet, h);
    build_music(sheet, h);
    const Source src(PICTURES[s_picture], SOURCE, SOURCE);
    for (Cover *c : s_cycled) {
        c->show(src.picture, false);
    }
    play_track(false);
    lv_timer_create([](lv_timer_t *) { if (s_shown) show_stats(); }, 500, nullptr);  // once a fade has let its old picture go
}

void pictures_shown(bool shown)
{
    if (shown == s_shown || s_backdrop == nullptr) {
        return;
    }
    s_shown = shown;
    if (shown) {
        const Source src(PICTURES[s_track], SOURCE, SOURCE);
        s_backdrop->show(src.picture);
    } else {
        drop_variant();
        s_backdrop->clear();  // half a megabyte, while nobody looks at it
    }
    show_stats();
}

void pictures_next_cover() { next_cover(); }
void pictures_next_track() { next_track(); }

// 0: the kit's cover; 1: an opaque picture, as corners baked into the colour
// under them would be; 2: alpha all through; 3: LVGL's image radius; 4: a frame
// that clips its corners.
void pictures_redraw_mode(int mode)
{
    drop_variant();
    if (mode <= 0 || s_music == nullptr) {
        return;
    }
    const lv_color_format_t cf = mode == 2 ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    s_variant_buf = lv_draw_buf_create(MUSIC_COVER, MUSIC_COVER, cf, LV_STRIDE_AUTO);
    if (s_variant_buf == nullptr) {
        return;
    }
    const Source src(PICTURES[s_track], MUSIC_COVER, MUSIC_COVER);
    if (src.pixels == nullptr) {
        return;
    }
    const std::int32_t stride = static_cast<std::int32_t>(s_variant_buf->header.stride);
    for (std::int32_t y = 0; y < MUSIC_COVER; ++y) {
        std::memcpy(s_variant_buf->data + y * stride, src.pixels + y * MUSIC_COVER, MUSIC_COVER * 2);
    }
    constexpr std::int32_t R = 16;
    if (mode == 2) {
        lv_opa_t *a = s_variant_buf->data + stride * MUSIC_COVER;
        for (std::int32_t y = 0; y < MUSIC_COVER; ++y) {
            for (std::int32_t x = 0; x < MUSIC_COVER; ++x) {
                const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                const float cx = std::clamp(px, static_cast<float>(R), static_cast<float>(MUSIC_COVER - R));
                const float cy = std::clamp(py, static_cast<float>(R), static_cast<float>(MUSIC_COVER - R));
                const float d  = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
                a[y * stride / 2 + x] = static_cast<lv_opa_t>(std::clamp(static_cast<float>(R) - d + 0.5f, 0.0f, 1.0f) * 255.0f);
            }
        }
    }
    lv_obj_t *parent = lv_obj_get_parent(s_music->obj());
    lv_obj_t *img    = nullptr;
    if (mode == 4) {
        s_variant = lv_obj_create(parent);
        lv_obj_remove_style_all(s_variant);
        lv_obj_set_style_radius(s_variant, R, 0);
        lv_obj_set_style_clip_corner(s_variant, true, 0);
        img = lv_image_create(s_variant);
    } else {
        s_variant = img = lv_image_create(parent);
        if (mode == 3) {
            lv_obj_set_style_radius(img, R, 0);
        }
    }
    lv_obj_set_size(s_variant, MUSIC_COVER, MUSIC_COVER);
    lv_obj_set_pos(s_variant, MUSIC_X, MUSIC_Y);
    lv_image_set_src(img, s_variant_buf);
    lv_obj_add_flag(s_music->obj(), LV_OBJ_FLAG_HIDDEN);
}

void pictures_redraw()
{
    lv_obj_invalidate(s_variant != nullptr ? s_variant : s_music->obj());
}

void pictures_bench_done() { drop_variant(); }

// Each way of rounding the music cover's corners, redrawn a hundred times over
// its backdrop: steadier than a bench phase on a desktop, where one frame of so
// small a redraw is too quick to time.
void pictures_measure_corners()
{
    if (!s_shown) {
        return;
    }
    const char *names[] = {"kit cover", "baked", "all alpha", "image radius", "clip corner"};
    for (int mode = 0; mode < 5; ++mode) {
        pictures_redraw_mode(mode);
        for (int i = 0; i < 5; ++i) {
            pictures_redraw();
            lv_refr_now(nullptr);
        }
        std::uint64_t took = 0;
        for (int i = 0; i < 100; ++i) {
            lv_delay_ms(12);  // the last frame handed over, so only this one's drawing is timed
            pictures_redraw();
            const std::uint64_t t0 = perf::now_us();
            lv_refr_now(nullptr);
            took += perf::now_us() - t0;
        }
        std::printf("corners, %-13s %6.3f ms a redraw of the %d px cover\n", names[mode],
                    static_cast<double>(took) / 100000.0, static_cast<int>(MUSIC_COVER));
    }
    pictures_redraw_mode(-1);
}

}  // namespace ui::lab
