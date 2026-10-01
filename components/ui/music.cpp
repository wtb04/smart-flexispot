#include "ui_internal.h"

#include <algorithm>
#include <cstdio>

// The speaker's music over the whole screen: the cover large, what plays and
// how far, and its controls, on the cover's own colour. Closed by itself when
// nothing plays, or a video takes the card, which the cinema view is for.
namespace ui::detail {
namespace {
constexpr std::int32_t PAD          = 40;
constexpr std::int32_t COVER        = media::kLargeArtSize;
constexpr std::int32_t COVER_X      = 80;
constexpr std::int32_t COVER_RADIUS = 24;
constexpr std::int32_t TEXT_GAP     = 64;
constexpr std::int32_t LINE_GAP     = 12;
constexpr std::int32_t PROGRESS_H   = 10;
constexpr std::int32_t TRANSPORT_H  = 88;
constexpr std::int32_t SIDE_W       = 120;  // the track before and after
constexpr std::int32_t PLAY_W       = 200;
constexpr std::int32_t VOLUME_H     = 56;
constexpr std::int32_t VOLUME_INSET = 24;
constexpr std::uint8_t VOLUME_FILL_MIX = 64;  // of the text's colour into the bar's
constexpr std::int32_t PAUSED_MARK  = 120;
constexpr lv_opa_t     PAUSED_DIM   = LV_OPA_50;
constexpr std::uint8_t TINT_MIX     = 46;   // of the cover's colour into the background
constexpr std::uint8_t SURFACE_MIX  = 26;   // of the text's colour into it, for what is on it
constexpr int          TINT_STEP    = 7;    // every so many pixels of the cover, for its colour
constexpr std::uint32_t TICK_MS     = 500;
// The card's cover, when there is no large one, grown to the large one's size.
constexpr std::uint32_t SMALL_SCALE = LV_SCALE_NONE * media::kLargeArtSize / media::kArtSize;

lv_obj_t      *s_view    = nullptr;
lv_obj_t      *s_cover   = nullptr;
lv_obj_t      *s_paused  = nullptr;
lv_image_dsc_t s_cover_dsc{};
const void    *s_large   = nullptr;  // the large cover, or null for the card's
lv_obj_t      *s_source  = nullptr;
lv_obj_t      *s_title   = nullptr;
lv_obj_t      *s_artist  = nullptr;
std::int32_t   s_artist_y[2] = {};  // under a title of one line, and of two
lv_obj_t      *s_bar     = nullptr;
lv_obj_t      *s_elapsed = nullptr;
lv_obj_t      *s_total   = nullptr;
lv_obj_t      *s_play    = nullptr;
lv_obj_t      *s_steer[3] = {};      // before, play, after
lv_obj_t      *s_volume  = nullptr;
lv_obj_t      *s_volume_fill  = nullptr;
lv_obj_t      *s_volume_level = nullptr;
bool           s_volume_held  = false;
lv_timer_t    *s_tick    = nullptr;
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

lv_obj_t *button(const char *text, std::int32_t w, lv_event_cb_t on_click)
{
    lv_obj_t *b = theme::make_button(s_view, text, theme::panel_light, fonts::size_32());
    lv_obj_set_size(b, w, TRANSPORT_H);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, nullptr);
    return b;
}

void steer(MediaAction action)
{
    if (media_remote() && s_handlers.media != nullptr) {
        s_handlers.media(action);
    }
}

// The cover's colour, from a pixel in so many, mixed into the background: the
// view takes on the record's colour without its picture behind the text.
void tint_from(const void *pixels, std::int32_t side)
{
    lv_color_t ink = lv_color_hex(theme::background);
    if (pixels != nullptr) {
        const auto  *px = static_cast<const std::uint16_t *>(pixels);
        std::uint32_t r = 0, g = 0, b = 0, n = 0;
        for (std::int32_t i = 0; i < side * side; i += TINT_STEP) {
            const std::uint16_t v = px[i];
            r += (v >> 11) & 0x1f;
            g += (v >> 5) & 0x3f;
            b += v & 0x1f;
            ++n;
        }
        const lv_color_t average = lv_color_make(static_cast<std::uint8_t>(r * 255 / (31 * n)),
                                                 static_cast<std::uint8_t>(g * 255 / (63 * n)),
                                                 static_cast<std::uint8_t>(b * 255 / (31 * n)));
        ink = lv_color_mix(average, lv_color_hex(theme::background), TINT_MIX);
    }
    lv_obj_set_style_bg_color(s_view, ink, 0);
    // What is on it takes its colour too, lighter, rather than the pages' own.
    const lv_color_t surface = pixels != nullptr ? lv_color_mix(lv_color_hex(theme::text), ink, SURFACE_MIX)
                                                 : lv_color_hex(theme::panel_light);
    for (lv_obj_t *part : {s_steer[0], s_steer[2], s_volume, s_bar}) {
        lv_obj_set_style_bg_color(part, surface, 0);
    }
    lv_obj_set_style_bg_color(s_volume_fill, lv_color_mix(lv_color_hex(theme::text), surface, VOLUME_FILL_MIX), 0);
}

// The large cover when there is one, else the card's grown to its size.
void show_cover()
{
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
        lv_obj_set_hidden(s_cover, false);
        tint_from(s_large, COVER);
        return;
    }
    const void *small = s_media_art != nullptr ? lv_image_get_src(s_media_art) : nullptr;
    lv_obj_set_hidden(s_cover, small == nullptr);
    if (small != nullptr) {
        lv_image_set_src(s_cover, small);
        lv_image_set_scale(s_cover, SMALL_SCALE);
        const auto *dsc = static_cast<const lv_image_dsc_t *>(small);
        tint_from(dsc->data, media::kArtSize);
    } else {
        tint_from(nullptr, 0);
    }
}

void show_volume(int percent)
{
    lv_obj_set_width(s_volume_fill, lv_obj_get_width(s_volume) * percent / 100);
    char text[12];
    std::snprintf(text, sizeof(text), "%d", percent);
    theme::set_text(s_volume_level, text);
}

void tick(lv_timer_t *)
{
    if (!s_has_track_shown || media_is_video()) {
        close_music();  // it stopped, or the cinema view is for this
        return;
    }
    if (s_large == nullptr && s_media_art != nullptr && lv_image_get_src(s_media_art) != lv_image_get_src(s_cover)) {
        show_cover();  // the card's cover changed with the track
    }
    theme::set_text(s_source, lv_label_get_text(s_media_source));
    const char *title = lv_label_get_text(s_media_title);
    theme::set_text(s_title, title);
    lv_point_t size{};
    lv_text_get_size(&size, title, fonts::size_48(), 0, 0, lv_obj_get_width(s_title), LV_TEXT_FLAG_NONE);
    lv_obj_set_y(s_artist, s_artist_y[size.y > lv_font_get_line_height(fonts::size_48()) ? 1 : 0]);
    theme::set_text(s_artist, lv_label_get_text(s_media_artist));

    const bool known = s_duration_s > 0;
    for (lv_obj_t *part : {s_bar, s_elapsed, s_total}) {
        lv_obj_set_hidden(part, !known);
    }
    if (known) {
        const int at = media_position_now();
        lv_bar_set_range(s_bar, 0, s_duration_s);
        lv_bar_set_value(s_bar, std::min(at, s_duration_s), LV_ANIM_OFF);
        write_clock(s_elapsed, at);
        write_clock(s_total, s_duration_s);
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
    lv_obj_get_coords(s_volume, &bar);
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

void build_text(std::int32_t x, std::int32_t y, std::int32_t w)
{
    s_source = theme::make_accent_label(s_view, "", fonts::size_22());
    lv_obj_set_pos(s_source, x, y);
    y += lv_font_get_line_height(fonts::size_22()) + LINE_GAP;
    s_title = line(theme::text, fonts::size_48(), w, 2);
    lv_obj_set_pos(s_title, x, y);
    s_artist_y[0] = y + lv_font_get_line_height(fonts::size_48()) + LINE_GAP;
    s_artist_y[1] = y + 2 * lv_font_get_line_height(fonts::size_48()) + LINE_GAP;
    s_artist      = line(theme::secondary, fonts::size_28(), w, 1);
    lv_obj_set_pos(s_artist, x, s_artist_y[0]);
}

// Under the text, from the bottom of the cover up: the volume, the controls,
// and the progress over them.
void build_controls(std::int32_t x, std::int32_t bottom, std::int32_t w)
{
    const std::int32_t volume_y    = bottom - VOLUME_H;
    const std::int32_t transport_y = volume_y - BUTTON_GAP - TRANSPORT_H;
    const std::int32_t times_y     = transport_y - 2 * BUTTON_GAP - lv_font_get_line_height(fonts::size_20());
    const std::int32_t bar_y       = times_y - LINE_GAP - PROGRESS_H;

    s_bar = lv_bar_create(s_view);
    lv_obj_set_size(s_bar, w, PROGRESS_H);
    lv_obj_set_pos(s_bar, x, bar_y);
    theme::style_panel(s_bar, theme::panel_light, PROGRESS_H / 2);
    theme::fill_accent(s_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, PROGRESS_H / 2, LV_PART_INDICATOR);
    s_elapsed = theme::make_label(s_view, "0:00", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_elapsed, x, times_y);
    s_total = theme::make_label(s_view, "0:00", theme::secondary, fonts::size_20());
    lv_obj_set_width(s_total, w);
    lv_obj_set_style_text_align(s_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_total, x, times_y);

    const std::int32_t row_w = 2 * SIDE_W + PLAY_W + 2 * BUTTON_GAP;
    const std::int32_t row_x = x + (w - row_w) / 2;
    s_steer[0] = button(LV_SYMBOL_PREV, SIDE_W, [](lv_event_t *) { steer(MediaAction::Previous); });
    lv_obj_set_pos(s_steer[0], row_x, transport_y);
    s_play = button(LV_SYMBOL_PLAY, PLAY_W, [](lv_event_t *) { media_toggle_play(); });
    theme::fill_accent(s_play);
    lv_obj_set_pos(s_play, row_x + SIDE_W + BUTTON_GAP, transport_y);
    s_steer[1] = s_play;
    s_steer[2] = button(LV_SYMBOL_NEXT, SIDE_W, [](lv_event_t *) { steer(MediaAction::Next); });
    lv_obj_set_pos(s_steer[2], row_x + SIDE_W + PLAY_W + 2 * BUTTON_GAP, transport_y);

    s_volume = lv_obj_create(s_view);
    theme::style_panel(s_volume, theme::panel_light, theme::radius::control);
    lv_obj_set_size(s_volume, w, VOLUME_H);
    lv_obj_set_pos(s_volume, x, volume_y);
    lv_obj_set_style_pad_all(s_volume, 0, 0);
    lv_obj_set_style_clip_corner(s_volume, true, 0);
    lv_obj_set_scrollable(s_volume, false);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(s_volume, volume_touched, code, nullptr);
    }
    s_volume_fill = lv_obj_create(s_volume);
    theme::style_panel(s_volume_fill, theme::panel_light, 0);
    lv_obj_set_style_bg_color(
        s_volume_fill, lv_color_mix(lv_color_hex(theme::text), lv_color_hex(theme::panel_light), VOLUME_FILL_MIX), 0);
    lv_obj_set_size(s_volume_fill, 0, VOLUME_H);
    lv_obj_set_pos(s_volume_fill, 0, 0);
    lv_obj_set_clickable(s_volume_fill, false);
    lv_obj_t *speaker = theme::make_label(s_volume, LV_SYMBOL_VOLUME_MAX, theme::text, fonts::size_28());
    lv_obj_align(speaker, LV_ALIGN_LEFT_MID, VOLUME_INSET, 0);
    lv_obj_set_clickable(speaker, false);
    s_volume_level = theme::make_label(s_volume, "", theme::text, fonts::size_28());
    lv_obj_align(s_volume_level, LV_ALIGN_RIGHT_MID, -VOLUME_INSET, 0);
    lv_obj_set_clickable(s_volume_level, false);
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
    build_text(x, cover_y, w);
    build_controls(x, cover_y + COVER, w);

    // The way back, as the other fullscreen views have it, and the desk.
    lv_obj_t *close = theme::make_chip(s_view, "");
    theme::make_mark(close, &icons::collapse_icon);
    lv_obj_set_ext_click_area(close, theme::space::s);
    lv_obj_add_event_cb(close, [](lv_event_t *) { close_music(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -PAD, PAD);
    lv_obj_t *desk = add_desk_shortcuts(s_view, PAD, PAD, theme::panel);

    s_tick = lv_timer_create(tick, TICK_MS, nullptr);
    lv_timer_pause(s_tick);
    s_music = add_view({"music", ViewKind::Fullscreen, s_view,
                        [] {
                            show_cover();
                            lv_timer_resume(s_tick);
                            tick(s_tick);
                        },
                        [] { lv_timer_pause(s_tick); }});
    for (lv_obj_t *control : {close, desk}) {
        fade_when_idle(s_music, control);
    }
}

void apply_music_cover(const void *pixels)
{
    s_large = pixels;
    if (s_view != nullptr && view_open(s_music)) {
        show_cover();
    }
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
