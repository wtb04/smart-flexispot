#include "ui_internal.h"

#include "status_model.h"

#include "room_model.h"
#include "topics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

// Jellyfin fullscreen, for watching, in two columns as wide as the picture is:
// the picture and what plays beside it, how far along the picture's foot, and
// under them the volume under the picture and the controls under the words.
// Ten seconds back or on is a swipe across the picture, as on the card; the
// lights, the low desk and the screen's timer are quick actions over the view.
// The screen goes dark once left alone, and lights again when there is an intro
// to skip or an episode to go on to.
namespace ui::detail {
namespace {
constexpr std::int32_t SIDE_MARGIN   = (720 - media::kLargeArtSize) / 2;  // as the music view's
constexpr std::int32_t ROW_GAP       = 40;   // between the picture's row and the controls'
constexpr std::int32_t STILL_W       = media::kStillW;
constexpr std::int32_t STILL_H       = media::kStillH;
constexpr std::int32_t STILL_RADIUS  = 22;
constexpr std::int32_t TEXT_GAP      = 72;
constexpr lv_opa_t     PAUSED_DIM    = LV_OPA_50;  // as the card's cover
constexpr std::int32_t PROGRESS_H    = 10;
constexpr std::int32_t LINE_GAP      = 10;
// The music view's buttons: the volume and the subtitles under the picture,
// before, play and after under the words.
constexpr std::int32_t TRANSPORT_H   = 88;
constexpr std::int32_t PLAY_W        = 200;
constexpr std::int32_t GRID_GAP      = 24;
constexpr std::int32_t SKIP_W        = 290;  // over the picture's corner, as on a screen,
constexpr std::int32_t SKIP_H        = 84;   // as large as it is to be tapped from the sofa
constexpr std::int32_t SKIP_INSET    = 20;
constexpr std::int32_t PAUSED_MARK   = 104;  // the round play mark over a paused picture
constexpr int          SEEK_STEP_S   = 10;
constexpr std::int32_t VOLUME_INSET  = 28;   // its speaker and level from its ends
constexpr std::uint8_t VOLUME_FILL_MIX = 64;  // of the text's colour into the bar's
constexpr time_t       CLOCK_SET     = 1'700'000'000;  // any earlier and the clock is not set yet
constexpr std::uint32_t TICK_MS      = 500;
constexpr std::uint32_t DARK_AFTER_MS = 15 * units::kMsPerSecond;

lv_obj_t     *s_view    = nullptr;
lv_obj_t     *s_still   = nullptr;
lv_image_dsc_t s_still_dsc{};
lv_obj_t     *s_series  = nullptr;
lv_obj_t     *s_title   = nullptr;
lv_obj_t     *s_episode = nullptr;
std::int32_t  s_text_top = 0;  // level with the top of the picture
lv_obj_t     *s_bar     = nullptr;
lv_obj_t     *s_elapsed = nullptr;
lv_obj_t     *s_total   = nullptr;
lv_obj_t     *s_play    = nullptr;
lv_obj_t     *s_skip    = nullptr;
lv_obj_t     *s_paused  = nullptr;  // the play mark over a paused picture
bool          s_swiped  = false;    // the release that ends a swipe is not a tap
lv_obj_t     *s_steer[2] = {};      // the episode before, and after
lv_obj_t     *s_volume  = nullptr;  // the slider, filled as far as the level
lv_obj_t     *s_volume_fill  = nullptr;
lv_obj_t     *s_volume_level = nullptr;
bool          s_volume_held  = false;  // a finger on it: what it shows is what it sets
lv_obj_t     *s_ends    = nullptr;  // when it will end, by the clock
lv_obj_t     *s_subtitles = nullptr;
std::uint32_t s_stills_shown = UINT32_MAX;  // the model's count of stills when last drawn
bool          s_auto_off = true;    // the screen goes dark when left alone
lv_obj_t     *s_low_chip    = nullptr;  // quick actions in the row over the view
lv_obj_t     *s_lights_chip = nullptr;
lv_obj_t     *s_screen_chip = nullptr;
lv_timer_t   *s_tick    = nullptr;
std::uint32_t s_woke_at = 0;  // lit for an intro to skip: kept lit a while from then
bool          s_skip_was_offered = false;

ViewId s_cinema = kNoView;

void close_cinema()
{
    close_view(s_cinema);
}

// From the top of the picture down, the episode under a title of one line or two.
void place_text(const char *title)
{
    const std::int32_t line_h = lv_font_get_line_height(fonts::size_48());
    lv_point_t         size{};
    lv_text_get_size(&size, title, fonts::size_48(), 0, 0, lv_obj_get_width(s_title), LV_TEXT_FLAG_NONE);
    const std::int32_t title_h  = size.y > line_h ? 2 * line_h : line_h;
    const std::int32_t series_h = lv_font_get_line_height(fonts::size_22());
    const std::int32_t top      = s_text_top;
    lv_obj_set_y(s_series, top);
    lv_obj_set_y(s_title, top + series_h + LINE_GAP);
    lv_obj_set_y(s_episode, top + series_h + LINE_GAP + title_h + LINE_GAP);
}

/** The artist line holds the series, then its season and episode. */
void show_text()
{
    const char *title  = media_state().title;
    const char *artist = media_state().artist;
    const char *split  = std::strchr(artist, '\n');
    if (std::strcmp(title, lv_label_get_text(s_title)) != 0) {
        theme::set_text(s_title, title);
        place_text(title);
    }
    if (split == nullptr) {
        theme::set_text(s_series, artist);
        theme::set_text(s_episode, "");
        return;
    }
    char series[96];
    std::snprintf(series, sizeof(series), "%.*s", static_cast<int>(split - artist), artist);
    theme::set_text(s_series, series);
    theme::set_text(s_episode, split + 1);
}

void show_progress()
{
    const bool known = media_state().duration_s > 0;
    lv_obj_set_hidden(s_bar, !known);
    lv_obj_set_hidden(s_elapsed, !known);
    lv_obj_set_hidden(s_total, !known);
    if (known) {
        const int at = media_position_now();
        lv_bar_set_range(s_bar, 0, media_state().duration_s);
        lv_bar_set_value(s_bar, at, LV_ANIM_OFF);
        write_clock(s_elapsed, at);
        write_clock(s_total, media_state().duration_s);
    }
    // Ends at, by the clock, once the clock is known.
    const time_t now = std::time(nullptr);
    lv_obj_set_hidden(s_ends, !known || now < CLOCK_SET);
    if (known && now >= CLOCK_SET) {
        const time_t ends = now + (media_state().duration_s - media_position_now());
        std::tm      local{};
        localtime_r(&ends, &local);
        char text[24];
        std::strftime(text, sizeof(text), "Ends at %H:%M", &local);
        theme::set_text(s_ends, text);
    }
}

void keep_screen()
{
    const bool offered = media_skip_offer().text != nullptr;
    if (offered && !s_skip_was_offered) {
        s_woke_at = lv_tick_get();
        set_screen_state(true);
    }
    s_skip_was_offered = offered;
    const bool untouched = lv_display_get_inactive_time(nullptr) >= DARK_AFTER_MS &&
                           lv_tick_elaps(s_woke_at) >= DARK_AFTER_MS;
    if (s_auto_off && status_state().screen_on && untouched && !offered) {
        set_screen_state(false);
    }
}

void show_volume(int percent)
{
    lv_obj_set_width(s_volume_fill, lv_obj_get_width(s_volume) * percent / 100);
    char text[12];
    std::snprintf(text, sizeof(text), "%d", percent);
    theme::set_text(s_volume_level, text);
}

void show_still(const void *pixels)
{
    lv_obj_set_hidden(s_still, pixels == nullptr);
    if (pixels == nullptr) {
        return;
    }
    const std::uint32_t bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    s_still_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_still_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_still_dsc.header.w      = STILL_W;
    s_still_dsc.header.h      = STILL_H;
    s_still_dsc.header.stride = STILL_W * bytes;
    s_still_dsc.data_size     = STILL_W * STILL_H * bytes;
    s_still_dsc.data          = static_cast<const std::uint8_t *>(pixels);
    lv_image_set_src(s_still, &s_still_dsc);
    lv_obj_invalidate(s_still);
}

// What plays, as the model has it, as it changes.
void show_media()
{
    if (!media_is_video()) {
        close_cinema();  // the film ended, or something else took over the card
        return;
    }
    show_text();
    const bool     playing = media_state().playing;
    const lv_opa_t dim     = playing ? static_cast<lv_opa_t>(LV_OPA_TRANSP) : PAUSED_DIM;
    if (lv_obj_get_style_image_recolor_opa(s_still, LV_PART_MAIN) != dim) {
        lv_obj_set_style_image_recolor(s_still, lv_color_hex(theme::background), 0);
        lv_obj_set_style_image_recolor_opa(s_still, dim, 0);
    }
    show_progress();
    theme::set_text(lv_obj_get_child(s_play, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_hidden(s_paused, playing);
    theme::set_usable(s_volume, media_state().volume >= 0);
    if (media_state().volume >= 0 && !s_volume_held) {
        show_volume(media_state().volume);
    }
    const MediaState &media = media_state();
    for (lv_obj_t *control : {s_play, s_skip}) {
        theme::set_usable(control, media.remote);
    }
    // Faded where there is no episode that side, so the grid stays as it is.
    theme::set_usable(s_steer[0], media.remote && media.before);
    theme::set_usable(s_steer[1], media.remote && media.after);
    theme::fill_accent_or(s_subtitles, media.subtitles_shown, theme::panel_light);
    theme::set_usable(s_subtitles, media.subtitles_available);
    if (media.stills != s_stills_shown) {
        s_stills_shown = media.stills;
        show_still(media.still);
    }
}

// What goes on with the time: how far, the intro or credits to skip, and the
// screen going dark; and what is not followed yet, the room and the episodes.
void tick(lv_timer_t *)
{
    show_progress();
    const char *skip = media_skip_offer().text;
    lv_obj_set_hidden(s_skip, skip == nullptr);
    if (skip != nullptr) {
        theme::set_text(lv_obj_get_child(s_skip, 0), skip);
    }
    light_chrome_chip(s_screen_chip, s_auto_off);
    keep_screen();
}

lv_obj_t *button(lv_obj_t *parent, const char *text, std::int32_t w, std::int32_t h,
                 lv_event_cb_t on_click, const lv_font_t *font = fonts::size_32())
{
    lv_obj_t *b = theme::make_button(parent, text, theme::panel_light, font);
    lv_obj_set_size(b, w, h);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, nullptr);
    return b;
}

/** A drawn mark in a button, in the text's colour. */
void mark(lv_obj_t *button, const lv_image_dsc_t *icon)
{
    lv_obj_t *image = lv_image_create(button);
    lv_image_set_src(image, icon);
    lv_obj_set_style_image_recolor(image, lv_color_hex(theme::text), 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
    lv_obj_center(image);
    lv_obj_set_clickable(image, false);
}

lv_obj_t *line(lv_obj_t *parent, std::uint32_t colour, const lv_font_t *font, std::int32_t w,
               std::int32_t lines)
{
    lv_obj_t *label = theme::make_label(parent, "", colour, font);
    lv_obj_set_width(label, w);
    lv_obj_set_height(label, lines * lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

// A tap on the picture pauses or plays, as tapping a video does; a swipe across
// it goes ten seconds on to the left or back to the right, as on the card.
void picture_touched(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
        if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) {
            s_swiped = true;
            media_seek_by(direction == LV_DIR_LEFT ? SEEK_STEP_S : -SEEK_STEP_S);
        }
        return;
    }
    if (!std::exchange(s_swiped, false)) {
        media_toggle_play();
    }
}

void build_film(std::int32_t x, std::int32_t y)
{
    lv_obj_t *frame = lv_obj_create(s_view);
    lv_obj_set_size(frame, STILL_W, STILL_H);
    theme::style_panel(frame, theme::panel, STILL_RADIUS);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_pos(frame, x, y);
    lv_obj_set_scrollable(frame, false);
    lv_obj_add_event_cb(frame, picture_touched, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(frame, picture_touched, LV_EVENT_GESTURE, nullptr);
    lv_obj_set_gesture_bubble(frame, false);  // else LVGL takes it past here to the screen
    s_still = lv_image_create(frame);
    lv_obj_center(s_still);
    lv_obj_set_hidden(s_still, true);

    s_paused = lv_obj_create(frame);
    theme::style_panel(s_paused, theme::background, theme::radius::pill);
    lv_obj_set_style_bg_opa(s_paused, LV_OPA_70, 0);
    lv_obj_set_size(s_paused, PAUSED_MARK, PAUSED_MARK);
    lv_obj_center(s_paused);
    lv_obj_set_clickable(s_paused, false);
    lv_obj_set_scrollable(s_paused, false);
    lv_obj_t *play = theme::make_label(s_paused, LV_SYMBOL_PLAY, theme::text, fonts::size_48());
    lv_obj_center(play);
    lv_obj_set_hidden(s_paused, true);

    // An intro to skip, or an episode to go on to, in the picture's corner.
    s_skip = button(frame, "Next episode", SKIP_W, SKIP_H, [](lv_event_t *) { media_skip(); });
    lv_obj_align(s_skip, LV_ALIGN_BOTTOM_RIGHT, -SKIP_INSET, -SKIP_INSET);
    lv_obj_set_hidden(s_skip, true);
}

void build_text(std::int32_t x, std::int32_t w)
{
    s_series  = line(s_view, theme::secondary, fonts::size_22(), w, 1);
    s_title   = line(s_view, theme::text, fonts::size_48(), w, 2);
    s_episode = line(s_view, theme::secondary, fonts::size_28(), w, 1);
    for (lv_obj_t *part : {s_series, s_title, s_episode}) {
        lv_obj_set_x(part, x);
    }
    place_text("");
}

// One bar, filled as loud as it plays, dragged or tapped anywhere along to set it.
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
    s_volume_held = true;
    if (percent != media_state().volume) {
        media_set_volume(percent);
        show_volume(percent);
    }
}

void build_volume(std::int32_t x, std::int32_t y, std::int32_t w)
{
    s_volume = lv_obj_create(s_view);
    theme::style_panel(s_volume, theme::panel_light, theme::radius::control);
    lv_obj_set_size(s_volume, w, TRANSPORT_H);
    lv_obj_set_pos(s_volume, x, y);
    lv_obj_set_style_pad_all(s_volume, 0, 0);
    lv_obj_set_style_clip_corner(s_volume, true, 0);
    lv_obj_set_scrollable(s_volume, false);
    lv_obj_add_event_cb(s_volume, volume_touched, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_volume, volume_touched, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_volume, volume_touched, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_volume, volume_touched, LV_EVENT_PRESS_LOST, nullptr);

    s_volume_fill = lv_obj_create(s_volume);
    // A level, not something on, so not the accent.
    theme::style_panel(s_volume_fill, theme::panel_light, 0);
    lv_obj_set_style_bg_color(s_volume_fill,
                              lv_color_mix(lv_color_hex(theme::text), lv_color_hex(theme::panel_light), VOLUME_FILL_MIX), 0);
    lv_obj_set_size(s_volume_fill, 0, TRANSPORT_H);
    lv_obj_set_pos(s_volume_fill, 0, 0);
    lv_obj_set_clickable(s_volume_fill, false);

    lv_obj_t *speaker = theme::make_label(s_volume, LV_SYMBOL_VOLUME_MAX, theme::text, fonts::size_28());
    lv_obj_align(speaker, LV_ALIGN_LEFT_MID, VOLUME_INSET, 0);
    lv_obj_set_clickable(speaker, false);
    s_volume_level = theme::make_label(s_volume, "", theme::text, fonts::size_28());
    lv_obj_align(s_volume_level, LV_ALIGN_RIGHT_MID, -VOLUME_INSET, 0);
    lv_obj_set_clickable(s_volume_level, false);
}

void preset(int index)
{
    desk_go_to(index);
}

// How far, along the foot of the picture beside it.
void build_progress(std::int32_t x, std::int32_t foot, std::int32_t w)
{
    const std::int32_t times_y = foot - lv_font_get_line_height(fonts::size_20());
    const std::int32_t bar_y   = times_y - LINE_GAP - PROGRESS_H;
    s_bar = lv_bar_create(s_view);
    lv_obj_set_size(s_bar, w, PROGRESS_H);
    lv_obj_set_pos(s_bar, x, bar_y);
    theme::style_panel(s_bar, theme::panel_light, PROGRESS_H / 2);
    theme::fill_accent(s_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, PROGRESS_H / 2, LV_PART_INDICATOR);
    s_elapsed = theme::make_label(s_view, "0:00", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_elapsed, x, times_y);
    for (lv_obj_t **label : {&s_total, &s_ends}) {
        *label = theme::make_label(s_view, "", theme::secondary, fonts::size_20());
        lv_obj_set_width(*label, w);
        lv_obj_set_pos(*label, x, times_y);
    }
    lv_obj_set_style_text_align(s_total, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(s_ends, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(s_ends, true);
}

std::int32_t side_width(std::int32_t w)
{
    return (w - PLAY_W - 2 * GRID_GAP) / 2;
}

// Before, play and after under the words, as wide as they are.
void build_transport(std::int32_t x, std::int32_t y, std::int32_t w)
{
    const std::int32_t side_w = side_width(w);
    s_steer[0] = button(s_view, LV_SYMBOL_PREV, side_w, TRANSPORT_H,
                        [](lv_event_t *) { media_action(MediaAction::Previous); });
    lv_obj_set_pos(s_steer[0], x, y);
    s_play = button(s_view, LV_SYMBOL_PLAY, PLAY_W, TRANSPORT_H, [](lv_event_t *) { media_toggle_play(); });
    theme::fill_accent(s_play);
    lv_obj_set_pos(s_play, x + side_w + GRID_GAP, y);
    s_steer[1] = button(s_view, LV_SYMBOL_NEXT, side_w, TRANSPORT_H,
                        [](lv_event_t *) { media_action(MediaAction::Next); });
    lv_obj_set_pos(s_steer[1], x + w - side_w, y);
}

// The volume and the subtitles under the picture, the subtitles as wide as a
// button beside play.
void build_sound(std::int32_t x, std::int32_t y, std::int32_t w)
{
    const std::int32_t side_w = side_width(w);
    build_volume(x, y, w - side_w - GRID_GAP);
    s_subtitles = button(s_view, "", side_w, TRANSPORT_H, [](lv_event_t *) {
        media_toggle_subtitles();
    });
    mark(s_subtitles, &icons::subtitles_icon);
    lv_obj_set_pos(s_subtitles, x + w - side_w, y);
}
}  // namespace

void build_cinema(lv_obj_t *screen)
{
    const Layout l = layout();
    s_view = lv_obj_create(screen);
    lv_obj_set_size(s_view, l.screen_w, l.screen_h);
    lv_obj_set_pos(s_view, 0, 0);
    theme::style_panel(s_view, theme::background, 0);
    lv_obj_set_scrollable(s_view, false);  // a drag across the picture is a swipe, not a scroll
    lv_obj_set_hidden(s_view, true);

    // The two rows in the middle of the height, as far in from the sides as the
    // music view is.
    const std::int32_t top   = (l.screen_h - STILL_H - ROW_GAP - TRANSPORT_H) / 2;
    const std::int32_t row_y = top + STILL_H + ROW_GAP;
    const std::int32_t x     = SIDE_MARGIN + STILL_W + TEXT_GAP;
    const std::int32_t w     = l.screen_w - x - SIDE_MARGIN;
    build_film(SIDE_MARGIN, top);
    s_text_top = top;
    build_text(x, w);
    build_progress(x, top + STILL_H, w);
    build_sound(SIDE_MARGIN, row_y, STILL_W);
    build_transport(x, row_y, w);

    s_tick = lv_timer_create(tick, TICK_MS, nullptr);
    lv_timer_pause(s_tick);
    s_cinema = add_view({"cinema", ViewKind::Fullscreen, s_view,
                         [] {
                             s_woke_at          = lv_tick_get();
                             s_skip_was_offered = false;
                             lv_timer_resume(s_tick);
                             tick(s_tick);
                         },
                         [] { lv_timer_pause(s_tick); }});
    subscribe(Topic::Media, s_cinema, show_media);
    Chrome chrome = add_fullscreen_chrome(s_cinema, s_view, [](lv_event_t *) { close_cinema(); });
    s_low_chip    = add_chrome_chip(chrome, &icons::desk_lowest_icon, [](lv_event_t *) { preset(ULTRA_LOW_PRESET); });
    s_lights_chip = add_chrome_chip(chrome, &icons::bulb_icon, [](lv_event_t *) {
        if (s_handlers.lights != nullptr) {
            s_handlers.lights();
        }
    });
    subscribe(Topic::Desk, s_cinema, [] {
        light_chrome_chip(s_low_chip, desk_state().preset_active[ULTRA_LOW_PRESET]);
        show_desk_travel(s_low_chip, desk_state().travelling == ULTRA_LOW_PRESET);
    });
    subscribe(Topic::Lights, s_cinema, [] { light_chrome_chip(s_lights_chip, lights_state().on); });
    // The screen going dark by itself, lit while it does.
    s_screen_chip = add_chrome_chip(chrome, &icons::screen_timer_icon, [](lv_event_t *) {
        s_auto_off = !s_auto_off;
        tick(s_tick);
    });
}

bool cinema_open()
{
    return view_open(s_cinema);
}

void open_cinema()
{
    open_view(s_cinema);
}
}  // namespace ui::detail
