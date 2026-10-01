#include "ui_internal.h"

#include <algorithm>
#include <cstring>

// The speaker's music over the whole screen, as the cinema view has a film:
// the cover large, what plays and how far, its controls, the volume and the
// favourites, all in the cover's own colours. Closed by itself when nothing
// plays, or a video takes the card. What plays, its cover and its times change
// together, as the card's do, and how far it is glides rather than ticks.
namespace ui::detail {
namespace {
constexpr std::int32_t PAD          = 40;
constexpr std::int32_t COVER        = media::kLargeArtSize;
constexpr std::int32_t COVER_X      = 80;
constexpr std::int32_t COVER_RADIUS = 28;
constexpr std::int32_t TEXT_GAP     = 80;
constexpr std::int32_t LINE_GAP     = 10;
constexpr std::int32_t TEXT_TO_BAR  = 48;   // under the artist, when the title leaves room
constexpr std::int32_t PROGRESS_H   = 6;
constexpr std::int32_t PLAY_D       = 112;  // round, in the cover's colour
constexpr std::int32_t SIDE_D       = 96;   // the track before and after, a bare mark to tap
constexpr std::int32_t TRANSPORT_GAP = 48;  // between the three, across
constexpr std::int32_t ROW_GAP      = 28;   // between the rows of controls
constexpr std::int32_t VOLUME_H     = 56;   // to touch: the line in it is thin
constexpr std::int32_t KNOB_D       = 22;
constexpr std::int32_t SPEAKER_W    = 56;   // the mark before the line
constexpr std::int32_t PICKS_D      = 64;
constexpr std::uint8_t PRESSED_MIX  = 40;   // of the text's colour into the surface, under a finger
constexpr std::int32_t PAUSED_MARK  = 120;
constexpr lv_opa_t     PAUSED_DIM   = LV_OPA_50;
constexpr std::uint32_t GLIDE_MS    = 33;   // the bar looked at, moved only when its end would move
constexpr std::uint32_t CHECK_MS    = 500;  // still playing, and still music
// The card's cover, when there is no large one, grown to the large one's size.
constexpr std::uint32_t SMALL_SCALE = LV_SCALE_NONE * media::kLargeArtSize / media::kArtSize;

// The cover's colours: its most vivid hue, as dark as the pages for behind,
// lighter for what is on it, and bright for what the accent marks elsewhere.
constexpr int      PALETTE_STEP   = 7;   // every so many pixels of the cover
constexpr int      VIVID_CHROMA   = 24;  // of 255: below it a pixel counts as grey
constexpr int      VIVID_SHARE    = 8;   // percent of the pixels that must have a colour
constexpr uint8_t  BACK_S = 70, BACK_V = 24;
constexpr uint8_t  SURFACE_S = 50, SURFACE_V = 36;
constexpr uint8_t  ACCENT_S_MIN = 45, ACCENT_S_MAX = 75, ACCENT_V = 96;
constexpr uint8_t  ACCENT_S_STEP = 5;
constexpr int      ACCENT_LUMA   = 140;  // of 255: as light as this, to read on the dark

struct Palette {
    lv_color_t back, surface, accent;
};

Palette pages_palette()
{
    return {lv_color_hex(theme::background), lv_color_hex(theme::panel_light), lv_color_hex(theme::primary)};
}

// Each pixel weighted by how much colour it has, so a cover's grey and black do
// not wash its hue out; a cover with little colour keeps the pages' own.
Palette palette_of(const std::uint16_t *pixels, int count)
{
    std::uint64_t r = 0, g = 0, b = 0, weight = 0;
    int           vivid = 0, seen = 0;
    for (int i = 0; i < count; i += PALETTE_STEP, ++seen) {
        const std::uint16_t v  = pixels[i];
        const int           pr = ((v >> 11) & 0x1f) * 255 / 31;
        const int           pg = ((v >> 5) & 0x3f) * 255 / 63;
        const int           pb = (v & 0x1f) * 255 / 31;
        const int           chroma = std::max({pr, pg, pb}) - std::min({pr, pg, pb});
        if (chroma < VIVID_CHROMA) {
            continue;
        }
        ++vivid;
        r += static_cast<std::uint64_t>(pr) * chroma;
        g += static_cast<std::uint64_t>(pg) * chroma;
        b += static_cast<std::uint64_t>(pb) * chroma;
        weight += chroma;
    }
    if (seen == 0 || vivid * 100 < seen * VIVID_SHARE) {
        return pages_palette();
    }
    const lv_color_hsv_t hue = lv_color_rgb_to_hsv(static_cast<std::uint8_t>(r / weight),
                                                   static_cast<std::uint8_t>(g / weight),
                                                   static_cast<std::uint8_t>(b / weight));
    // Blue and violet are dark at any brightness: paler until they read.
    std::uint8_t saturation = std::clamp<std::uint8_t>(hue.s, ACCENT_S_MIN, ACCENT_S_MAX);
    lv_color_t   accent     = lv_color_hsv_to_rgb(hue.h, saturation, ACCENT_V);
    while (lv_color_luminance(accent) < ACCENT_LUMA && saturation > ACCENT_S_STEP) {
        saturation -= ACCENT_S_STEP;
        accent = lv_color_hsv_to_rgb(hue.h, saturation, ACCENT_V);
    }
    return {lv_color_hsv_to_rgb(hue.h, BACK_S, BACK_V), lv_color_hsv_to_rgb(hue.h, SURFACE_S, SURFACE_V), accent};
}

lv_obj_t      *s_view    = nullptr;
lv_obj_t      *s_cover   = nullptr;
lv_obj_t      *s_paused  = nullptr;
lv_image_dsc_t s_cover_dsc{};
const void    *s_large   = nullptr;  // the large cover, or null for the card's
const void    *s_shown   = nullptr;  // the cover on show, and coloured by
bool           s_cover_new = true;   // a cover came, maybe into the same buffer
lv_obj_t      *s_source  = nullptr;
lv_obj_t      *s_title   = nullptr;
lv_obj_t      *s_artist  = nullptr;
std::int32_t   s_text_top = 0;     // no higher than the cover
std::int32_t   s_text_end = 0;     // where the text ends, over the progress
lv_obj_t      *s_bar     = nullptr;
lv_obj_t      *s_elapsed = nullptr;
lv_obj_t      *s_total   = nullptr;
lv_obj_t      *s_play    = nullptr;
lv_obj_t      *s_steer[3] = {};      // before, play, after
lv_obj_t      *s_picks   = nullptr;
lv_obj_t      *s_volume  = nullptr;
lv_obj_t      *s_volume_line  = nullptr;
lv_obj_t      *s_volume_fill  = nullptr;
lv_obj_t      *s_volume_knob  = nullptr;
bool           s_volume_held  = false;
int            s_duration_shown = -1;
std::int32_t   s_bar_end      = -1;  // where the bar's end was last drawn, in pixels
int            s_elapsed_s    = -1;
lv_timer_t    *s_glide   = nullptr;
lv_timer_t    *s_check   = nullptr;
ViewId         s_music   = kNoView;

void close_music()
{
    close_view(s_music);
}

lv_obj_t *line(std::uint32_t colour, const lv_font_t *font, std::int32_t w, std::int32_t lines)
{
    lv_obj_t *label = theme::make_label(s_view, "", colour, font);
    lv_obj_set_width(label, w);
    lv_obj_set_height(label, lines * lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

// Round; with nothing behind the mark but while a finger is on it, unless filled.
lv_obj_t *button(const char *text, std::int32_t d, const lv_font_t *font, bool filled, lv_event_cb_t on_click)
{
    lv_obj_t *b = theme::make_button(s_view, text, theme::panel_light, font);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, theme::radius::pill, 0);
    if (!filled) {
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    }
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, nullptr);
    return b;
}

void steer(MediaAction action)
{
    if (media_remote() && s_handlers.media != nullptr) {
        s_handlers.media(action);
    }
}

void paint(const Palette &colours)
{
    lv_obj_set_style_bg_color(s_view, colours.back, 0);
    lv_obj_set_style_bg_color(s_volume_line, colours.surface, 0);
    lv_obj_set_style_bg_color(s_bar, colours.surface, 0);
    const lv_color_t pressed = lv_color_mix(lv_color_hex(theme::text), colours.surface, PRESSED_MIX);
    for (lv_obj_t *bare : {s_steer[0], s_steer[2], s_picks}) {
        lv_obj_set_style_bg_color(bare, colours.surface, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(bare, LV_OPA_COVER, LV_STATE_PRESSED);
    }
    lv_obj_set_style_bg_color(s_play, pressed, LV_STATE_PRESSED);
    // What the accent marks on the pages takes the cover's own instead, light
    // enough that what is on it is in the dark of the background.
    lv_obj_set_style_bg_color(s_play, colours.accent, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_play, 0), colours.back, 0);
    lv_obj_set_style_bg_color(s_bar, colours.accent, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_source, colours.accent, 0);
}

// The large cover when there is one, else the card's grown to its size, and
// the view in its colours. Only when it changed: drawing it again is the
// biggest thing on the screen.
void show_cover()
{
    const void *small = s_media_art != nullptr && !lv_obj_is_hidden(s_media_art)
                            ? lv_image_get_src(s_media_art)
                            : nullptr;
    const void *wanted = s_large != nullptr ? s_large : small;
    if (wanted == s_shown && !s_cover_new) {
        return;
    }
    s_shown     = wanted;
    s_cover_new = false;
    lv_obj_set_hidden(s_cover, wanted == nullptr);
    if (s_large != nullptr) {
        const std::uint32_t bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
        s_cover_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_cover_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_cover_dsc.header.w      = COVER;
        s_cover_dsc.header.h      = COVER;
        s_cover_dsc.header.stride = COVER * bytes;
        s_cover_dsc.data_size     = COVER * COVER * bytes;
        s_cover_dsc.data          = static_cast<const std::uint8_t *>(s_large);
        lv_image_set_src(s_cover, &s_cover_dsc);
        lv_image_set_scale(s_cover, LV_SCALE_NONE);
        paint(palette_of(static_cast<const std::uint16_t *>(s_large), COVER * COVER));
    } else if (small != nullptr) {
        lv_image_set_src(s_cover, small);
        lv_image_set_scale(s_cover, SMALL_SCALE);
        const auto *dsc = static_cast<const lv_image_dsc_t *>(small);
        paint(palette_of(reinterpret_cast<const std::uint16_t *>(dsc->data), media::kArtSize * media::kArtSize));
    } else {
        paint(pages_palette());
    }
    lv_obj_invalidate(s_view);
}

void show_volume(int percent)
{
    const std::int32_t at = lv_obj_get_width(s_volume_line) * percent / 100;
    lv_obj_set_width(s_volume_fill, at);
    lv_obj_set_x(s_volume_knob, lv_obj_get_x(s_volume_line) + at - KNOB_D / 2);
}

// How far it is: the bar drawn again only when its end moves a pixel, the
// elapsed time only when a second has gone.
void glide(lv_timer_t *)
{
    if (s_duration_s <= 0) {
        return;
    }
    const int          at_ms = media_position_ms_now();
    const std::int32_t end   = static_cast<std::int32_t>(static_cast<std::int64_t>(at_ms) * lv_obj_get_width(s_bar) /
                                                         (static_cast<std::int64_t>(s_duration_s) * units::kMsPerSecond));
    if (end != s_bar_end) {
        s_bar_end = end;
        lv_bar_set_value(s_bar, at_ms, LV_ANIM_OFF);
    }
    const int at_s = at_ms / units::kMsPerSecond;
    if (at_s != s_elapsed_s) {
        s_elapsed_s = at_s;
        write_clock(s_elapsed, at_s);
    }
}

void check(lv_timer_t *)
{
    if (!s_has_track_shown || media_is_video()) {
        close_music();  // it stopped, or the cinema view is for this
    }
}

// The text sits over the progress, as one with it, rising for a title of two
// lines until it meets the top of the cover.
void place_text(const char *title)
{
    const std::int32_t line_h = lv_font_get_line_height(fonts::size_48());
    lv_point_t         size{};
    lv_text_get_size(&size, title, fonts::size_48(), 0, 0, lv_obj_get_width(s_title), LV_TEXT_FLAG_NONE);
    const std::int32_t title_h = size.y > line_h ? 2 * line_h : line_h;
    const std::int32_t source_h = lv_font_get_line_height(fonts::size_22());
    const std::int32_t artist_h = lv_font_get_line_height(fonts::size_28());
    const std::int32_t text_h   = source_h + LINE_GAP + title_h + LINE_GAP + artist_h;
    const std::int32_t top      = std::max(s_text_top, s_text_end - TEXT_TO_BAR - text_h);
    lv_obj_set_y(s_source, top);
    lv_obj_set_y(s_title, top + source_h + LINE_GAP);
    lv_obj_set_y(s_artist, top + source_h + LINE_GAP + title_h + LINE_GAP);
}
}  // namespace

void refresh_music()
{
    if (s_view == nullptr || !view_open(s_music)) {
        return;
    }
    theme::set_text(s_source, lv_label_get_text(s_media_source));
    const char *title = lv_label_get_text(s_media_title);
    if (std::strcmp(title, lv_label_get_text(s_title)) != 0) {
        theme::set_text(s_title, title);
        place_text(title);
    }
    theme::set_text(s_artist, lv_label_get_text(s_media_artist));
    show_cover();

    const bool known = s_duration_s > 0;
    for (lv_obj_t *part : {s_bar, s_elapsed, s_total}) {
        lv_obj_set_hidden(part, !known);
    }
    if (known) {
        if (s_duration_s != s_duration_shown) {
            s_duration_shown = s_duration_s;
            lv_bar_set_range(s_bar, 0, s_duration_s * units::kMsPerSecond);
            write_clock(s_total, s_duration_s);
        }
        s_bar_end   = -1;  // where it is may have jumped
        s_elapsed_s = -1;
        glide(nullptr);
    }

    theme::set_text(lv_obj_get_child(s_play, 0), s_playing_shown ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_hidden(s_paused, s_playing_shown);
    const lv_opa_t dim = s_playing_shown ? static_cast<lv_opa_t>(LV_OPA_TRANSP) : PAUSED_DIM;
    if (lv_obj_get_style_image_recolor_opa(s_cover, LV_PART_MAIN) != dim) {
        lv_obj_set_style_image_recolor(s_cover, lv_color_hex(theme::background), 0);
        lv_obj_set_style_image_recolor_opa(s_cover, dim, 0);
    }
    for (lv_obj_t *control : s_steer) {
        theme::set_usable(control, media_remote());
    }
    theme::set_usable(s_volume, s_media_volume >= 0);
    if (s_media_volume >= 0 && !s_volume_held) {
        show_volume(s_media_volume);
    }
}

namespace {
void volume_touched(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_volume_held = false;
        return;
    }
    lv_point_t at{};
    lv_indev_get_point(lv_indev_active(), &at);
    lv_area_t bar{};
    lv_obj_get_coords(s_volume_line, &bar);
    const int percent = std::clamp(static_cast<int>((at.x - bar.x1) * 100 / lv_area_get_width(&bar)), 0, 100);
    s_volume_held     = true;
    if (percent == s_media_volume) {
        return;
    }
    s_media_volume = percent;
    show_volume(percent);
    if (s_handlers.media_volume != nullptr) {
        s_handlers.media_volume(percent);
    }
}

void build_cover(std::int32_t y)
{
    lv_obj_t *frame = lv_obj_create(s_view);
    lv_obj_set_size(frame, COVER, COVER);
    theme::style_panel(frame, theme::panel, COVER_RADIUS);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_pos(frame, COVER_X, y);
    lv_obj_set_scrollable(frame, false);
    // Tapping the record pauses or plays it, as the cinema view's picture does.
    lv_obj_add_event_cb(frame, [](lv_event_t *) { media_toggle_play(); }, LV_EVENT_CLICKED, nullptr);
    s_cover = lv_image_create(frame);
    lv_obj_center(s_cover);
    lv_obj_set_clickable(s_cover, false);
    lv_obj_set_hidden(s_cover, true);

    s_paused = lv_obj_create(frame);
    theme::style_panel(s_paused, theme::background, theme::radius::pill);
    lv_obj_set_style_bg_opa(s_paused, LV_OPA_70, 0);
    lv_obj_set_size(s_paused, PAUSED_MARK, PAUSED_MARK);
    lv_obj_center(s_paused);
    lv_obj_set_clickable(s_paused, false);
    lv_obj_set_scrollable(s_paused, false);
    lv_obj_center(theme::make_label(s_paused, LV_SYMBOL_PLAY, theme::text, fonts::size_48()));
    lv_obj_set_hidden(s_paused, true);
}

// Between the top of the cover and the progress: s_text_top to s_text_end.
void build_text(std::int32_t x, std::int32_t w)
{
    s_source = theme::make_label(s_view, "", theme::primary, fonts::size_22());
    s_title  = line(theme::text, fonts::size_48(), w, 2);
    s_artist = line(theme::secondary, fonts::size_28(), w, 1);
    for (lv_obj_t *part : {s_source, s_title, s_artist}) {
        lv_obj_set_x(part, x);
    }
    place_text("");
}

lv_obj_t *round_part(lv_obj_t *parent, std::int32_t w, std::int32_t h, std::uint32_t colour)
{
    lv_obj_t *part = lv_obj_create(parent);
    theme::style_panel(part, colour, theme::radius::pill);
    lv_obj_set_size(part, w, h);
    lv_obj_set_clickable(part, false);
    lv_obj_set_scrollable(part, false);
    return part;
}

// A thin line to slide along, its mark before it; the whole height is to touch.
void build_volume(std::int32_t x, std::int32_t y, std::int32_t w)
{
    s_volume = lv_obj_create(s_view);
    lv_obj_set_size(s_volume, w, VOLUME_H);
    lv_obj_set_pos(s_volume, x, y);
    lv_obj_set_style_bg_opa(s_volume, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_volume, 0, 0);
    lv_obj_set_style_pad_all(s_volume, 0, 0);
    lv_obj_set_scrollable(s_volume, false);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(s_volume, volume_touched, code, nullptr);
    }
    lv_obj_t *speaker = theme::make_label(s_volume, LV_SYMBOL_VOLUME_MAX, theme::secondary, fonts::size_28());
    lv_obj_align(speaker, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_clickable(speaker, false);
    const std::int32_t line_w = w - SPEAKER_W - KNOB_D / 2;
    s_volume_line = round_part(s_volume, line_w, PROGRESS_H, theme::panel_light);
    lv_obj_align(s_volume_line, LV_ALIGN_LEFT_MID, SPEAKER_W, 0);
    s_volume_fill = round_part(s_volume, 0, PROGRESS_H, theme::text);
    lv_obj_align(s_volume_fill, LV_ALIGN_LEFT_MID, SPEAKER_W, 0);
    s_volume_knob = round_part(s_volume, KNOB_D, KNOB_D, theme::text);
    lv_obj_align(s_volume_knob, LV_ALIGN_LEFT_MID, SPEAKER_W - KNOB_D / 2, 0);
}

// Under the text, from the bottom of the cover up: the volume and the
// favourites, the controls, and the progress over them.
void build_controls(std::int32_t x, std::int32_t bottom, std::int32_t w)
{
    const std::int32_t volume_y    = bottom - VOLUME_H;
    const std::int32_t transport_y = volume_y - ROW_GAP - PLAY_D;
    const std::int32_t times_y     = transport_y - ROW_GAP - lv_font_get_line_height(fonts::size_20());
    const std::int32_t bar_y       = times_y - LINE_GAP - PROGRESS_H;
    s_text_end                     = bar_y;

    s_bar = lv_bar_create(s_view);
    lv_obj_set_size(s_bar, w, PROGRESS_H);
    lv_obj_set_pos(s_bar, x, bar_y);
    theme::style_panel(s_bar, theme::panel_light, theme::radius::pill);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, theme::radius::pill, LV_PART_INDICATOR);
    s_elapsed = theme::make_label(s_view, "0:00", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_elapsed, x, times_y);
    s_total = theme::make_label(s_view, "0:00", theme::secondary, fonts::size_20());
    lv_obj_set_width(s_total, w);
    lv_obj_set_style_text_align(s_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_total, x, times_y);

    const std::int32_t middle = x + w / 2;
    const std::int32_t side_y = transport_y + (PLAY_D - SIDE_D) / 2;
    s_play = button(LV_SYMBOL_PLAY, PLAY_D, fonts::size_48(), true, [](lv_event_t *) { media_toggle_play(); });
    lv_obj_set_pos(s_play, middle - PLAY_D / 2, transport_y);
    s_steer[0] = button(LV_SYMBOL_PREV, SIDE_D, fonts::size_48(), false,
                        [](lv_event_t *) { steer(MediaAction::Previous); });
    lv_obj_set_pos(s_steer[0], middle - PLAY_D / 2 - TRANSPORT_GAP - SIDE_D, side_y);
    s_steer[1] = s_play;
    s_steer[2] = button(LV_SYMBOL_NEXT, SIDE_D, fonts::size_48(), false, [](lv_event_t *) { steer(MediaAction::Next); });
    lv_obj_set_pos(s_steer[2], middle + PLAY_D / 2 + TRANSPORT_GAP, side_y);

    // The favourites, to play another instead, at the volume's end.
    s_picks = button(LV_SYMBOL_LIST, PICKS_D, fonts::size_28(), false, [](lv_event_t *) { open_favourites(); });
    lv_obj_set_pos(s_picks, x + w - PICKS_D, volume_y + (VOLUME_H - PICKS_D) / 2);
    build_volume(x, volume_y, w - PICKS_D - BUTTON_GAP);
}
}  // namespace

void build_music(lv_obj_t *screen)
{
    const Layout l = layout();
    s_view = lv_obj_create(screen);
    lv_obj_set_size(s_view, l.screen_w, l.screen_h);
    lv_obj_set_pos(s_view, 0, 0);
    theme::style_panel(s_view, theme::background, 0);
    lv_obj_set_scrollable(s_view, false);
    lv_obj_set_hidden(s_view, true);

    const std::int32_t cover_y = (l.screen_h - COVER) / 2;
    build_cover(cover_y);
    const std::int32_t x = COVER_X + COVER + TEXT_GAP;
    const std::int32_t w = l.screen_w - x - COVER_X;
    s_text_top = cover_y;
    build_controls(x, cover_y + COVER, w);
    build_text(x, w);
    paint(pages_palette());

    // The way back, as the other fullscreen views have it, and the desk.
    lv_obj_t *close = theme::make_chip(s_view, "");
    theme::make_mark(close, &icons::collapse_icon);
    lv_obj_set_ext_click_area(close, theme::space::s);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_music(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -PAD, PAD);
    lv_obj_t *desk = add_desk_shortcuts(s_view, PAD, PAD, theme::panel);

    s_glide = lv_timer_create(glide, GLIDE_MS, nullptr);
    s_check = lv_timer_create(check, CHECK_MS, nullptr);
    lv_timer_pause(s_glide);
    lv_timer_pause(s_check);
    s_music = add_view({"music", ViewKind::Fullscreen, s_view,
                        [] {
                            s_cover_new      = true;  // coloured again, as the accent may have changed
                            s_duration_shown = -1;
                            lv_timer_resume(s_glide);
                            lv_timer_resume(s_check);
                            refresh_music();
                            update_view_clocks();
                        },
                        [] {
                            lv_timer_pause(s_glide);
                            lv_timer_pause(s_check);
                        }});
    for (lv_obj_t *control : {close, desk}) {
        fade_when_idle(s_music, control);
    }
    add_view_clock(s_music, s_view, close);
}

void apply_music_cover(const void *pixels)
{
    s_large     = pixels;
    s_cover_new = true;
}

bool music_open()
{
    return view_open(s_music);
}

void open_music()
{
    open_view(s_music);
}
}  // namespace ui::detail
