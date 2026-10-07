// Music and the cinema over the whole screen, as Apple's Now Playing has them:
// the picture large over a blurred backdrop of itself, what plays, how far,
// round fills either side of the accent capsule and a thick volume slider.
// Each closes itself when what plays is no longer its own, and gives its
// pictures' memory back on closing.

#include "kit_front_views.h"

#include "kit_fonts.h"
#include "media.h"
#include "media_model.h"
#include "room_model.h"
#include "screen_rules.h"
#include "topics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

namespace ui::kitfront::views {
namespace {
namespace ic = kit::glyph;
using namespace kit;
using detail::media_state;
using detail::MediaState;

constexpr std::int32_t RAIL_SPAN = 10000;  // a progress rail's steps: finer than its pixels
constexpr time_t       CLOCK_SET = 1'700'000'000;  // any earlier and the clock is not set yet

lv_obj_t *bare(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *line(lv_obj_t *parent, Role role, Ink ink, std::int32_t w)
{
    lv_obj_t *label = text(parent, role, "", ink);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

// A title of one line or two, as it needs, so what is under it sits close.
void set_title(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) == 0) {
        return;
    }
    const lv_font_t   *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    const std::int32_t lh   = lv_font_get_line_height(font);
    lv_point_t         size{};
    lv_text_get_size(&size, value, font, 0, 0, lv_obj_get_width(label), LV_TEXT_FLAG_NONE);
    lv_obj_set_height(label, size.y > lh ? 2 * lh : lh);
    lv_label_set_text(label, value);
}

// --- the volume: a thick slider, or down and up where it only steps ---

struct Volume : InPsram {
    lv_obj_t *root = nullptr, *slider = nullptr, *down = nullptr, *up = nullptr;
    bool      held = false;
};

void volume_step(lv_event_t *e)
{
    const bool        up    = lv_event_get_user_data(e) != nullptr;
    const MediaState &media = media_state();
    if (media.steps_volume || media.volume < 0) {
        detail::media_action(up ? MediaAction::VolumeUp : MediaAction::VolumeDown);
    } else {
        detail::media_set_volume(std::clamp(media.volume + (up ? 5 : -5), 0, 100));
    }
}

Volume *volume(lv_obj_t *parent, std::int32_t w)
{
    auto *v = new Volume;
    v->root = flow(parent, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_size(v->root, w, size::button_lg);
    lv_obj_set_flex_grow(v->root, 1);  // the room of a neighbour left out too
    lv_obj_set_flex_align(v->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    v->slider = fill_slider(v->root, w, ic::volume_up);
    lv_obj_set_width(v->slider, LV_PCT(100));
    lv_obj_set_gesture_bubble(v->slider, false);  // dragged a little down, it is still the volume
    lv_obj_add_event_cb(
        v->slider,
        [](lv_event_t *e) {
            auto *vol = static_cast<Volume *>(lv_event_get_user_data(e));
            switch (lv_event_get_code(e)) {
                case LV_EVENT_PRESSED: vol->held = true; break;
                case LV_EVENT_RELEASED:
                case LV_EVENT_PRESS_LOST: vol->held = false; break;
                case LV_EVENT_VALUE_CHANGED: detail::media_set_volume(lv_slider_get_value(vol->slider)); break;
                default: break;
            }
        },
        LV_EVENT_ALL, v);
    for (bool up : {false, true}) {
        lv_obj_t *b = button(v->root, Button::Raised, up ? ic::volume_up : ic::volume_down, nullptr);
        lv_obj_set_height(b, size::button_lg);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_add_event_cb(b, volume_step, LV_EVENT_CLICKED, up ? v : nullptr);
        (up ? v->up : v->down) = b;
    }
    return v;
}

void show_volume(Volume &v)
{
    const MediaState &media = media_state();
    lv_obj_set_hidden(v.root, !detail::media_shows_volume());
    set_usable(v.root, media.volume >= 0 || media.steps_volume);
    lv_obj_set_hidden(v.slider, media.steps_volume);
    lv_obj_set_hidden(v.down, !media.steps_volume);
    lv_obj_set_hidden(v.up, !media.steps_volume);
    if (media.volume >= 0 && !v.held && lv_slider_get_value(v.slider) != media.volume) {
        lv_slider_set_value(v.slider, media.volume, LV_ANIM_OFF);
    }
}

// How far into it, as a share of the rail's steps.
std::int32_t progress_steps()
{
    const std::int64_t whole = static_cast<std::int64_t>(media_state().duration_s) * 1000;
    return whole > 0 ? static_cast<std::int32_t>(detail::media_position_ms_now() * RAIL_SPAN / whole) : 0;
}

// Its line from x to x + w, level with the words over it: a rail's track stops
// short of its ends by the knob's overhang.
Rail *progress(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w)
{
    constexpr std::int32_t OVERHANG = 8;
    RailSpec spec;
    spec.length = w + 2 * OVERHANG;
    spec.max    = RAIL_SPAN;
    spec.quiet  = true;
    auto *rail  = new Rail(parent, spec);
    lv_obj_set_pos(rail->obj(), x - OVERHANG, y);
    return rail;
}

void show_picture(Cover &cover, Backdrop &backdrop, const void *pixels, std::int32_t w, std::int32_t h, bool animate)
{
    if (pixels == nullptr) {
        cover.clear(animate);
        backdrop.clear();
        return;
    }
    const Picture picture{pixels, w, h, 0};
    cover.show(picture, animate);
    backdrop.show(picture);
}

// --- music ---------------------------------------------------------------------

namespace music {
constexpr std::int32_t COVER = media::kLargeArtSize, X0 = 96, Y0 = (size::screen_h - COVER) / 2;
constexpr std::int32_t X = X0 + COVER + 72, W = size::screen_w - X - X0;
constexpr std::int32_t VOLUME_Y = Y0 + COVER - size::button_lg, TRANSPORT_H = 88, SIDE = 80;
constexpr std::int32_t TRANSPORT_Y = VOLUME_Y - space::xxl - TRANSPORT_H;
constexpr std::int32_t TIMES_Y = TRANSPORT_Y - space::xxl - 24, RAIL_Y = TIMES_Y - 30;

detail::ViewId s_view = detail::kNoView;
Backdrop      *s_backdrop = nullptr;
Cover         *s_cover = nullptr;
std::uint32_t  s_covers = UINT32_MAX;  // the model's counts of covers and large ones when last shown
std::uint32_t  s_larges = UINT32_MAX;
std::uint32_t  s_tinted = UINT32_MAX;  // and of covers when the backdrop was last made
lv_timer_t    *s_retry  = nullptr;
lv_obj_t      *s_source = nullptr, *s_title = nullptr, *s_artist = nullptr;
Rail          *s_rail = nullptr;
lv_obj_t      *s_elapsed = nullptr, *s_total = nullptr;
lv_obj_t      *s_prev = nullptr, *s_play = nullptr, *s_next = nullptr, *s_picks = nullptr;
Volume        *s_volume = nullptr;
lv_timer_t    *s_glide = nullptr;
int            s_elapsed_s = -1, s_total_s = -1;

// The rail drawn again only as its end moves a pixel, the time only as a second goes.
void glide(lv_timer_t *)
{
    if (media_state().duration_s <= 0) {
        return;
    }
    s_rail->set_value(progress_steps(), false);
    const int at = detail::media_position_now();
    if (std::exchange(s_elapsed_s, at) != at) {
        write_clock(s_elapsed, at);
    }
}

// The large cover, once it follows, takes the small one's place without a
// fade, and the backdrop it was made from stays: the colours are the same.
// Each is marked shown only once it is, and tried again soon when it found no room.
void show_cover()
{
    const MediaState &media = media_state();
    const bool   large  = media.large != nullptr;
    const void  *pixels = large ? media.large : media.placeholder ? nullptr : media.art;
    const std::int32_t w = large ? media::kLargeArtSize : media.art_width, h = large ? media::kLargeArtSize : media::kArtSize;
    bool failed = false;
    if (s_covers != media.covers || s_larges != media.larges) {
        const bool animate = s_covers != media.covers && lv_obj_is_visible(s_cover->obj());
        if (pixels == nullptr) {
            s_cover->clear(animate);
        } else if (!s_cover->show(Picture{pixels, w, h, 0}, animate)) {
            failed = true;
        }
        if (!failed) {
            s_covers = media.covers;
            s_larges = media.larges;
        }
    }
    if (s_tinted != media.covers) {
        if (pixels == nullptr) {
            s_backdrop->clear();
            s_tinted = media.covers;
        } else if (s_backdrop->show(Picture{pixels, w, h, 0})) {
            s_tinted = media.covers;
        } else {
            failed = true;
        }
    }
    if (failed) {
        again_soon(s_retry, [] {
            if (detail::view_open(s_view)) {
                show_cover();
            }
        });
    }
}

void show()
{
    const MediaState &media = media_state();
    if (!media.has_track || detail::media_is_video()) {
        detail::close_view(s_view);  // it stopped, or the cinema is for this
        return;
    }
    set_text_if(s_source, media.source);
    set_title(s_title, media.title);
    char artist[sizeof media.artist];
    std::snprintf(artist, sizeof artist, "%s", media.artist);
    if (char *nl = std::strchr(artist, '\n')) {
        *nl = '\0';
    }
    set_text_if(s_artist, artist);
    show_cover();

    const bool known = media.duration_s > 0;
    for (lv_obj_t *part : {s_rail->obj(), s_elapsed, s_total}) {
        lv_obj_set_hidden(part, !known);
    }
    if (known && std::exchange(s_total_s, media.duration_s) != media.duration_s) {
        write_clock(s_total, media.duration_s);
    }
    s_elapsed_s = -1;  // where it is may have jumped
    glide(nullptr);

    lv_obj_t *glyph = lv_obj_get_child(s_play, 0);
    set_icon(glyph, media.playing ? ic::pause : ic::play_arrow, true);
    lv_obj_set_hidden(s_prev, !media.remote || !media.tracks_back);
    lv_obj_set_hidden(s_play, !media.remote);
    lv_obj_set_hidden(s_next, !media.remote || !media.tracks_on);
    show_volume(*s_volume);
}

void build(lv_obj_t *)
{
    lv_obj_t *root = fullscreen_root();
    s_backdrop     = new Backdrop(root, size::screen_w, size::screen_h);
    s_view         = detail::add_view({"music", detail::ViewKind::Fullscreen, root,
                                       [] {
                                           s_covers  = UINT32_MAX;
                                           s_larges  = UINT32_MAX;
                                           s_tinted  = UINT32_MAX;
                                           s_total_s = -1;
                                           lv_timer_resume(s_glide);
                                       },
                                       [] {
                                           lv_timer_pause(s_glide);
                                           s_cover->release();
                                           s_backdrop->clear();
                                       }});

    s_cover = new Cover(root, COVER, COVER, Shape::Rounded, radius::tile);
    lv_obj_set_pos(s_cover->obj(), X0, Y0);
    lv_obj_add_flag(s_cover->obj(), LV_OBJ_FLAG_CLICKABLE);  // a tap on the record plays or pauses it
    lv_obj_add_event_cb(s_cover->obj(), [](lv_event_t *) { detail::media_toggle_play(); }, LV_EVENT_CLICKED, nullptr);

    // from the top of the cover down to the progress, the words sitting on it
    lv_obj_t *words = bare(root, W, RAIL_Y - space::l - Y0);
    lv_obj_set_pos(words, X, Y0);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(words, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(words, space::xs, 0);
    s_source = line(words, Role::Small, Ink::Secondary, W);
    s_title  = line(words, Role::H1, Ink::Primary, W);
    s_artist = line(words, Role::H3, Ink::Secondary, W);
    lv_obj_set_style_pad_bottom(s_source, space::xs, 0);

    s_rail = progress(root, X, RAIL_Y, W);
    s_elapsed = figures(root, "000:00", LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_elapsed, X, TIMES_Y);
    s_total = figures(root, "000:00", LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_total, X + W - lv_obj_get_style_width(s_total, LV_PART_MAIN), TIMES_Y);

    // round fills either side of the one accent capsule, as on the media card
    lv_obj_t *transport = flow(root, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_size(transport, W, TRANSPORT_H);
    lv_obj_set_pos(transport, X, TRANSPORT_Y);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_prev = fill_button(transport, ic::skip_previous, SIDE, 40);
    s_play = primary(transport, ic::play_arrow, 200, TRANSPORT_H);
    s_next = fill_button(transport, ic::skip_next, SIDE, 40);
    lv_obj_add_event_cb(s_prev, [](lv_event_t *) { detail::media_action(MediaAction::Previous); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_play, [](lv_event_t *) { detail::media_toggle_play(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_next, [](lv_event_t *) { detail::media_action(MediaAction::Next); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *bottom = flow(root, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_size(bottom, W, size::button_lg);
    lv_obj_set_pos(bottom, X, VOLUME_Y);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // the favourites, to play another instead
    s_picks = button(bottom, Button::Raised, ic::music_note, "Favourites");
    lv_obj_set_height(s_picks, size::button_lg);
    lv_obj_add_event_cb(s_picks, [](lv_event_t *e) { open_favourites(lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);
    popover_source(s_picks);
    lv_obj_update_layout(s_picks);
    s_volume = volume(bottom, W - lv_obj_get_width(s_picks) - space::m);
    lv_obj_move_to_index(s_picks, -1);

    s_glide = lv_timer_create(glide, 33, nullptr);
    lv_timer_pause(s_glide);
    detail::subscribe(Topic::Media, s_view, show);
    detail::subscribe(Topic::Picks, s_view, [] { lv_obj_set_hidden(s_picks, detail::media_pick_count() == 0); });
    chrome(s_view, root);
}
}  // namespace music

// --- the cinema ------------------------------------------------------------

namespace cinema {
constexpr std::int32_t STILL_W = 576, STILL_H = STILL_W * media::kStillH / media::kStillW;
constexpr std::int32_t ROW_GAP = 32, ROW_H = 88, SIDE = 80;
constexpr std::int32_t X0 = 96, Y0 = (size::screen_h - STILL_H - ROW_GAP - ROW_H) / 2;
constexpr std::int32_t X = X0 + STILL_W + 56, W = size::screen_w - X - X0;
constexpr std::int32_t ROW_Y = Y0 + STILL_H + ROW_GAP, TIMES_Y = Y0 + STILL_H - 20, RAIL_Y = TIMES_Y - 30;
constexpr int          SEEK_S = 10;
constexpr std::int64_t DARK_AFTER_MS = 15 * 1000;

detail::ViewId s_view = detail::kNoView;
Cover         *s_still = nullptr;
std::uint32_t  s_stills = UINT32_MAX;
lv_timer_t    *s_still_retry = nullptr;
lv_obj_t      *s_skip = nullptr;
lv_obj_t      *s_series = nullptr, *s_title = nullptr, *s_episode = nullptr;
Rail          *s_rail = nullptr;
lv_obj_t      *s_elapsed = nullptr, *s_total = nullptr, *s_ends = nullptr;
lv_obj_t      *s_back = nullptr, *s_play = nullptr, *s_on = nullptr, *s_subtitles = nullptr;
Volume        *s_volume = nullptr;
lv_obj_t      *s_low = nullptr, *s_lights = nullptr, *s_timer = nullptr;
lv_timer_t    *s_tick = nullptr;
bool           s_auto_off = true;  // the screen goes dark when left alone
bool           s_swiped   = false;  // the release that ends a swipe is not a tap

void show_progress()
{
    const MediaState &media = media_state();
    const bool        known = media.duration_s > 0;
    for (lv_obj_t *part : {s_rail->obj(), s_elapsed, s_total}) {
        lv_obj_set_hidden(part, !known);
    }
    const time_t now = detail::wall_now();
    lv_obj_set_hidden(s_ends, !known || now < CLOCK_SET);
    if (!known) {
        return;
    }
    const int at = detail::media_position_now();
    s_rail->set_value(progress_steps(), false);
    write_clock(s_elapsed, at);
    write_clock(s_total, media.duration_s);
    if (now >= CLOCK_SET) {
        const time_t ends = now + (media.duration_s - at);
        std::tm      local{};
        localtime_r(&ends, &local);
        char buf[24];
        std::strftime(buf, sizeof buf, "Ends at %H:%M", &local);
        set_text_if(s_ends, buf);
    }
}

// What goes on with the time: how far, and an intro or credits to skip.
void tick(lv_timer_t *)
{
    show_progress();
    const char *skip = detail::media_skip_offer().text;
    lv_obj_set_hidden(s_skip, skip == nullptr);
    if (skip != nullptr) {
        set_text_if(lv_obj_get_child(s_skip, 0), skip);
    }
    light(s_timer, s_auto_off);
}

// The artist line holds the series, then its season and episode.
void show_words()
{
    const MediaState &media = media_state();
    set_title(s_title, media.title);
    const char *split = std::strchr(media.artist, '\n');
    char        series[sizeof media.artist];
    std::snprintf(series, sizeof series, "%.*s",
                  static_cast<int>(split != nullptr ? split - media.artist : std::strlen(media.artist)), media.artist);
    set_text_if(s_series, series);
    set_text_if(s_episode, split != nullptr ? split + 1 : "");
}

void show()
{
    const MediaState &media = media_state();
    if (!detail::media_is_video()) {
        detail::close_view(s_view);  // the film ended, or something else plays
        return;
    }
    show_words();
    // marked shown once it is: without room, the still is tried again shortly
    if (s_stills != media.stills) {
        const bool animate = lv_obj_is_visible(s_still->obj());
        if (media.still == nullptr) {
            s_still->clear(animate);
            s_stills = media.stills;
        } else if (s_still->show(Picture{media.still, media::kStillW, media::kStillH, 0}, animate)) {
            s_stills = media.stills;
        } else {
            again_soon(s_still_retry, [] {
                if (detail::view_open(s_view)) {
                    show();
                }
            });
        }
    }
    tick(nullptr);
    set_icon(lv_obj_get_child(s_play, 0), media.playing ? ic::pause : ic::play_arrow, true);
    for (lv_obj_t *control : {s_back, s_play, s_on}) {
        lv_obj_set_hidden(control, !media.remote);
    }
    // faded until the position is known, so the row stays as it is
    set_usable(s_back, media.duration_s > 0);
    set_usable(s_on, media.duration_s > 0);
    show_volume(*s_volume);
    lv_obj_set_hidden(s_subtitles, !detail::media_shows_subtitles());
    set_usable(s_subtitles, media.subtitles_available);
    light(s_subtitles, media.subtitles_shown);
}

// A tap on the picture pauses or plays, as tapping a video does; a swipe across
// it goes on to the next episode to the left, or back to the one before to the right.
void still_touched(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        s_swiped                 = true;
        const lv_dir_t    dir    = lv_indev_get_gesture_dir(lv_indev_active());
        const MediaState &media  = media_state();
        if (dir == LV_DIR_BOTTOM) {
            detail::close_view(s_view);
        } else if ((dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) && media.remote &&
                   (dir == LV_DIR_LEFT ? media.after : media.before)) {
            detail::media_action(dir == LV_DIR_LEFT ? MediaAction::Next : MediaAction::Previous);
        }
        return;
    }
    if (!std::exchange(s_swiped, false)) {
        detail::media_toggle_play();
    }
}

// What the kit paints again on each restyle, kept as on black.
void paint_black(void *)
{
    lv_obj_set_style_bg_color(lv_obj_get_child(lv_obj_get_child(s_rail->obj(), 0), 0), lv_color_hex(ON_BLACK), 0);
    lv_obj_t *slider = s_volume->slider;
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, 40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(ON_BLACK), LV_PART_INDICATOR);
}

void build(lv_obj_t *)
{
    // black in either look, as a cinema is, with what is on it in the dark look's inks
    lv_obj_t *root = fullscreen_root(true);
    s_view         = detail::add_view({"cinema", detail::ViewKind::Fullscreen, root,
                                       [] {
                                           s_stills = UINT32_MAX;
                                           lv_timer_resume(s_tick);
                                       },
                                       [] {
                                           lv_timer_pause(s_tick);
                                           s_still->release();  // its buffer back, while no film shows
                                       }});

    s_still = new Cover(root, STILL_W, STILL_H, Shape::Rounded, 20, ic::aspect_ratio);
    lv_obj_t *still = s_still->obj();
    lv_obj_set_pos(still, X0, Y0);
    lv_obj_add_flag(still, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(still, still_touched, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(still, still_touched, LV_EVENT_GESTURE, nullptr);
    lv_obj_set_gesture_bubble(still, false);
    // an intro to skip, or an episode to go on to, in the picture's corner
    s_skip = button(still, Button::Raised, nullptr, "Next episode");
    lv_obj_set_size(s_skip, widest(Role::H3, {"Skip intro", "Next episode", "Next video"}) + 2 * space::xxl,
                    size::button_lg);
    lv_obj_align(s_skip, LV_ALIGN_BOTTOM_RIGHT, -space::l, -space::l);
    lv_obj_add_event_cb(s_skip, [](lv_event_t *) { detail::media_skip(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_skip, true);

    lv_obj_t *words = bare(root, W, RAIL_Y - space::l - Y0);
    lv_obj_set_pos(words, X, Y0);
    lv_obj_set_flex_flow(words, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(words, space::xs, 0);
    s_series  = line(words, Role::Small, Ink::Secondary, W);
    s_title   = line(words, Role::H1, Ink::Primary, W);
    s_episode = line(words, Role::Body, Ink::Secondary, W);
    lv_obj_set_style_pad_bottom(s_series, space::xs, 0);

    s_rail = progress(root, X, RAIL_Y, W);
    s_elapsed = figures(root, "000:00", LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_elapsed, X, TIMES_Y);
    s_ends = figures(root, "Ends at 00:00", LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_ends, X + (W - lv_obj_get_style_width(s_ends, LV_PART_MAIN)) / 2, TIMES_Y);
    s_total = figures(root, "000:00", LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_total, X + W - lv_obj_get_style_width(s_total, LV_PART_MAIN), TIMES_Y);

    // under the picture, the sound; under the words, the controls
    lv_obj_t *sound = flow(root, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_size(sound, STILL_W, ROW_H);
    lv_obj_set_pos(sound, X0, ROW_Y);
    lv_obj_set_flex_align(sound, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_volume    = volume(sound, STILL_W - SIDE - space::l);
    s_subtitles = fill_button(sound, ic::subtitles, SIDE, 34);
    lv_obj_add_event_cb(s_subtitles, [](lv_event_t *) { detail::media_toggle_subtitles(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *transport = flow(root, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_size(transport, W, ROW_H);
    lv_obj_set_pos(transport, X, ROW_Y);
    lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_back = fill_button(transport, ic::replay_10, SIDE, 40);
    s_play = primary(transport, ic::play_arrow, 200, ROW_H);
    s_on   = fill_button(transport, ic::forward_10, SIDE, 40);
    lv_obj_add_event_cb(s_back, [](lv_event_t *) { detail::media_seek_by(-SEEK_S); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_play, [](lv_event_t *) { detail::media_toggle_play(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_on, [](lv_event_t *) { detail::media_seek_by(SEEK_S); }, LV_EVENT_CLICKED, nullptr);

    s_tick = lv_timer_create(tick, 500, nullptr);
    lv_timer_pause(s_tick);
    detail::subscribe(Topic::Media, s_view, show);

    const Chrome over = chrome(s_view, root, true);
    s_low = desk_chip(over.left, ic::desk, ic::arrow_downward);
    lv_obj_add_event_cb(s_low, [](lv_event_t *) { detail::desk_go_to(detail::ULTRA_LOW_PRESET); }, LV_EVENT_CLICKED,
                        nullptr);
    s_lights = quick_action(over, ic::lightbulb);
    lv_obj_add_event_cb(
        s_lights,
        [](lv_event_t *) {
            if (detail::s_handlers.lights != nullptr) {
                detail::s_handlers.lights();
            }
        },
        LV_EVENT_CLICKED, nullptr);
    s_timer = quick_action(over, ic::bedtime);
    lv_obj_add_event_cb(
        s_timer,
        [](lv_event_t *) {
            s_auto_off = !s_auto_off;
            light(s_timer, s_auto_off);
        },
        LV_EVENT_CLICKED, nullptr);
    detail::subscribe(Topic::Desk, s_view, [] {
        light(s_low, detail::desk_state().preset_active[detail::ULTRA_LOW_PRESET]);
        set_usable(s_low, detail::desk_state().available);
    });
    detail::subscribe(Topic::Lights, s_view, [] { light(s_lights, detail::lights_state().on); });

    for (lv_obj_t *b : {s_skip, s_subtitles, s_back, s_on, s_volume->down, s_volume->up, s_low, s_lights, s_timer}) {
        on_black(b);
    }
    for (lv_obj_t *label : {s_title}) {
        ink_on_black(label, Ink::Primary);
    }
    for (lv_obj_t *label : {s_series, s_episode, s_elapsed, s_ends, s_total}) {
        ink_on_black(label, Ink::Secondary);
    }
    lv_obj_t *track = lv_obj_get_child(s_rail->obj(), 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(track, 20, 0);
    paint_black(nullptr);
    on_restyle(root, paint_black, nullptr);  // after the rail's and the slider's own
}
}  // namespace cinema
}  // namespace

void build_media(lv_obj_t *screen)
{
    cinema::build(screen);
    music::build(screen);
}

void open_music() { detail::open_view(music::s_view); }
void open_cinema() { detail::open_view(cinema::s_view); }
}  // namespace ui::kitfront::views

namespace ui::detail {
std::int64_t cinema_dark_after()
{
    using namespace kitfront::views::cinema;
    if (!view_open(s_view)) {
        return 0;
    }
    return s_auto_off ? DARK_AFTER_MS : screen_rules::kNever;
}

bool cinema_offers_skip()
{
    return view_open(kitfront::views::cinema::s_view) && media_skip_offer().text != nullptr;
}
}  // namespace ui::detail
