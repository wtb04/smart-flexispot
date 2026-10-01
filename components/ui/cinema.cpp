#include "ui_internal.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

// Jellyfin fullscreen, for watching: the film and its controls large, the desk
// and the lights beside them, and nothing else. The screen goes dark once left
// alone, and lights again when there is an intro to skip or an episode to go on to.
namespace ui::detail {
namespace {
constexpr std::int32_t PAD           = 40;
constexpr std::int32_t STILL_W       = media::kStillW;
constexpr std::int32_t STILL_H       = media::kStillH;
constexpr std::int32_t STILL_RADIUS  = 22;
constexpr std::int32_t TEXT_GAP      = 40;
constexpr lv_opa_t     PAUSED_DIM    = LV_OPA_50;  // as the card's cover
constexpr std::int32_t TOP_Y         = 120;  // under the row over every fullscreen view
constexpr std::int32_t PROGRESS_H    = 10;
constexpr std::int32_t LINE_GAP      = 12;
constexpr std::int32_t TRANSPORT_H   = 96;
constexpr std::int32_t STEP_W        = 150;
constexpr std::int32_t PLAY_W        = 220;
constexpr std::int32_t SKIP_W        = 220;  // over the picture's corner, as on a screen
constexpr std::int32_t SKIP_H        = 64;
constexpr std::int32_t SKIP_INSET    = 16;
constexpr std::int32_t PAUSED_MARK   = 104;  // the round play mark over a paused picture
constexpr std::int32_t ROOM_H        = 84;
constexpr int          SEEK_STEP_S   = 10;
constexpr std::int32_t VOLUME_W      = 320;  // the slider
constexpr std::int32_t VOLUME_INSET  = 28;   // its speaker and level from its ends
constexpr std::uint8_t VOLUME_FILL_MIX = 64;  // of the text's colour into the bar's
constexpr std::int32_t EPISODE_W     = 96;   // the episode before, and after
// The viewing height and the lights share the transport row's width with the
// two square ones, so both rows end in line; Stand and Sit are over the view.
constexpr std::int32_t TRANSPORT_W   = 2 * EPISODE_W + 2 * STEP_W + PLAY_W + 4 * BUTTON_GAP;
constexpr std::int32_t ROOM_W        = (TRANSPORT_W - 2 * ROOM_H - 3 * BUTTON_GAP) / 2;
constexpr time_t       CLOCK_SET     = 1'700'000'000;  // any earlier and the clock is not set yet
constexpr std::uint32_t TICK_MS      = 500;
constexpr std::uint32_t DARK_AFTER_MS = 15 * units::kMsPerSecond;

lv_obj_t     *s_view    = nullptr;
lv_obj_t     *s_still   = nullptr;
lv_image_dsc_t s_still_dsc{};
lv_obj_t     *s_series  = nullptr;
lv_obj_t     *s_title   = nullptr;
lv_obj_t     *s_episode = nullptr;
lv_obj_t     *s_bar     = nullptr;
lv_obj_t     *s_elapsed = nullptr;
lv_obj_t     *s_total   = nullptr;
lv_obj_t     *s_play    = nullptr;
lv_obj_t     *s_skip    = nullptr;
lv_obj_t     *s_paused  = nullptr;  // the play mark over a paused picture
std::int32_t  s_episode_y[2] = {};  // under a title of one line, and of two
lv_obj_t     *s_lights  = nullptr;
lv_obj_t     *s_volume  = nullptr;  // the slider, filled as far as the level
lv_obj_t     *s_volume_fill  = nullptr;
lv_obj_t     *s_volume_level = nullptr;
bool          s_volume_held  = false;  // a finger on it: what it shows is what it sets
lv_obj_t     *s_ends    = nullptr;  // when it will end, by the clock
bool          s_auto_off = true;    // the screen goes dark when left alone
lv_obj_t     *s_screen  = nullptr;  // the button that says so
lv_obj_t     *s_subtitles = nullptr;
bool          s_subtitles_available = false;
bool          s_subtitles_shown     = false;
std::int32_t  s_row_y   = 0;
lv_obj_t     *s_row[5]  = {};       // the episode before, back, play, on, the episode after
bool          s_neighbour[2] = {};  // before, after
lv_obj_t     *s_low     = nullptr;
lv_timer_t   *s_tick    = nullptr;
std::uint32_t s_woke_at = 0;  // lit for an intro to skip: kept lit a while from then
bool          s_skip_was_offered = false;

ViewId s_cinema = kNoView;

void close_cinema()
{
    close_view(s_cinema);
}

/** The artist line holds the series, then its season and episode. */
void show_text()
{
    const char *title  = lv_label_get_text(s_media_title);
    const char *artist = lv_label_get_text(s_media_artist);
    const char *split  = std::strchr(artist, '\n');
    theme::set_text(s_title, title);
    lv_point_t size{};
    lv_text_get_size(&size, title, fonts::size_48(), 0, 0, lv_obj_get_width(s_title), LV_TEXT_FLAG_NONE);
    const bool two_lines = size.y > lv_font_get_line_height(fonts::size_48());
    lv_obj_set_y(s_episode, s_episode_y[two_lines ? 1 : 0]);
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
    const bool known = s_duration_s > 0;
    lv_obj_set_hidden(s_bar, !known);
    lv_obj_set_hidden(s_elapsed, !known);
    lv_obj_set_hidden(s_total, !known);
    if (known) {
        const int at = media_position_now();
        lv_bar_set_range(s_bar, 0, s_duration_s);
        lv_bar_set_value(s_bar, at, LV_ANIM_OFF);
        write_clock(s_elapsed, at);
        write_clock(s_total, s_duration_s);
    }
    // Ends at, by the clock, once the clock is known.
    const time_t now = std::time(nullptr);
    lv_obj_set_hidden(s_ends, !known || now < CLOCK_SET);
    if (known && now >= CLOCK_SET) {
        const time_t ends = now + (s_duration_s - media_position_now());
        std::tm      local{};
        localtime_r(&ends, &local);
        char text[24];
        std::strftime(text, sizeof(text), "Ends at %H:%M", &local);
        theme::set_text(s_ends, text);
    }
}

/** The row closes up round the episode buttons it has to leave out. */
void place_row()
{
    std::int32_t x = PAD;
    for (int i = 0; i < static_cast<int>(std::size(s_row)); ++i) {
        const bool shown = (i != 0 || s_neighbour[0]) && (i != 4 || s_neighbour[1]);
        lv_obj_set_hidden(s_row[i], !shown);
        if (shown) {
            lv_obj_set_pos(s_row[i], x, s_row_y);
            // The width it was given: laid out it may not be yet, and reads 0.
            x += lv_obj_get_style_width(s_row[i], LV_PART_MAIN) + BUTTON_GAP;
        }
    }
}

void keep_screen()
{
    const bool offered = media_skip_text() != nullptr;
    if (offered && !s_skip_was_offered) {
        s_woke_at = lv_tick_get();
        set_screen_state(true);
    }
    s_skip_was_offered = offered;
    const bool untouched = lv_display_get_inactive_time(nullptr) >= DARK_AFTER_MS &&
                           lv_tick_elaps(s_woke_at) >= DARK_AFTER_MS;
    if (s_auto_off && s_screen_on && untouched && !offered) {
        set_screen_state(false);
    }
}

void show_volume(int percent)
{
    lv_obj_set_width(s_volume_fill, VOLUME_W * percent / 100);
    char text[12];
    std::snprintf(text, sizeof(text), "%d", percent);
    theme::set_text(s_volume_level, text);
}

void tick(lv_timer_t *)
{
    if (!media_is_video()) {
        close_cinema();  // the film ended, or something else took over the card
        return;
    }
    show_text();
    const lv_opa_t dim = s_playing_shown ? static_cast<lv_opa_t>(LV_OPA_TRANSP) : PAUSED_DIM;
    if (lv_obj_get_style_image_recolor_opa(s_still, LV_PART_MAIN) != dim) {
        lv_obj_set_style_image_recolor(s_still, lv_color_hex(theme::background), 0);
        lv_obj_set_style_image_recolor_opa(s_still, dim, 0);
    }
    show_progress();
    theme::set_text(lv_obj_get_child(s_play, 0), s_playing_shown ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_hidden(s_paused, s_playing_shown);
    const char *skip = media_skip_text();
    lv_obj_set_hidden(s_skip, skip == nullptr);
    if (skip != nullptr) {
        theme::set_text(lv_obj_get_child(s_skip, 0), skip);
    }
    for (lv_obj_t *control : {s_row[0], s_row[1], s_row[2], s_row[3], s_row[4], s_skip}) {
        theme::set_usable(control, media_remote());
    }
    theme::fill_accent_or(s_lights, s_lights_on, theme::panel_light);
    theme::fill_accent_or(s_screen, s_auto_off, theme::panel_light);
    theme::fill_accent_or(s_subtitles, s_subtitles_shown, theme::panel_light);
    theme::set_usable(s_subtitles, s_subtitles_available);
    theme::set_usable(s_volume, s_media_volume >= 0);
    if (s_media_volume >= 0 && !s_volume_held) {
        show_volume(s_media_volume);
    }
    theme::fill_accent_or(s_low, s_preset_active[ULTRA_LOW_PRESET], theme::panel_light);
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

void build_film(std::int32_t width)
{
    lv_obj_t *frame = lv_obj_create(s_view);
    lv_obj_set_size(frame, STILL_W, STILL_H);
    theme::style_panel(frame, theme::panel, STILL_RADIUS);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_pos(frame, PAD, TOP_Y);
    // Tapping the picture pauses or plays, as tapping a video does.
    lv_obj_add_event_cb(frame, [](lv_event_t *) { media_toggle_play(); }, LV_EVENT_CLICKED, nullptr);
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
    s_skip = button(frame, "Skip intro", SKIP_W, SKIP_H, [](lv_event_t *) { media_skip(); },
                    fonts::size_28());
    lv_obj_align(s_skip, LV_ALIGN_BOTTOM_RIGHT, -SKIP_INSET, -SKIP_INSET);
    lv_obj_set_hidden(s_skip, true);

    const std::int32_t x = PAD + STILL_W + TEXT_GAP;
    const std::int32_t w = width - x - PAD;
    std::int32_t       y = TOP_Y;
    s_series = line(s_view, theme::secondary, fonts::size_22(), w, 1);
    lv_obj_set_pos(s_series, x, y);
    y += lv_font_get_line_height(fonts::size_22()) + LINE_GAP;
    s_title = line(s_view, theme::text, fonts::size_48(), w, 2);
    lv_obj_set_pos(s_title, x, y);
    s_episode_y[0] = y + lv_font_get_line_height(fonts::size_48()) + LINE_GAP;
    s_episode_y[1] = y + 2 * lv_font_get_line_height(fonts::size_48()) + LINE_GAP;
    s_episode = line(s_view, theme::secondary, fonts::size_22(), w, 1);
    lv_obj_set_pos(s_episode, x, s_episode_y[0]);

    const std::int32_t bar_y =
        TOP_Y + STILL_H - PROGRESS_H - lv_font_get_line_height(fonts::size_20()) - LINE_GAP;
    s_bar = lv_bar_create(s_view);
    lv_obj_set_size(s_bar, w, PROGRESS_H);
    lv_obj_set_pos(s_bar, x, bar_y);
    theme::style_panel(s_bar, theme::panel_light, PROGRESS_H / 2);
    theme::fill_accent(s_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, PROGRESS_H / 2, LV_PART_INDICATOR);
    const std::int32_t times_y = bar_y + PROGRESS_H + LINE_GAP;
    s_elapsed = theme::make_label(s_view, "0:00", theme::secondary, fonts::size_20());
    lv_obj_set_pos(s_elapsed, x, times_y);
    s_total = theme::make_label(s_view, "0:00", theme::secondary, fonts::size_20());
    lv_obj_align(s_total, LV_ALIGN_TOP_RIGHT, -PAD, times_y);
    s_ends = theme::make_label(s_view, "", theme::secondary, fonts::size_20());
    lv_obj_set_width(s_ends, w);
    lv_obj_set_style_text_align(s_ends, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ends, x, times_y);
    lv_obj_set_hidden(s_ends, true);
}

void media_action(MediaAction action)
{
    if (!media_remote() && action != MediaAction::Subtitles) {
        return;
    }
    if (s_handlers.media != nullptr) {
        s_handlers.media(action);
    }
}

// At the row's far end: one bar, filled as loud as it plays, dragged or
// tapped anywhere along to set it.
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
    if (percent == s_media_volume) {
        return;
    }
    s_media_volume = percent;
    show_volume(percent);
    if (s_handlers.media_volume != nullptr) {
        s_handlers.media_volume(percent);
    }
}

void build_volume(std::int32_t y)
{
    const Layout l = layout();
    s_volume = lv_obj_create(s_view);
    theme::style_panel(s_volume, theme::panel_light, theme::radius::control);
    lv_obj_set_size(s_volume, VOLUME_W, TRANSPORT_H);
    lv_obj_set_pos(s_volume, l.screen_w - PAD - VOLUME_W, y);
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

void build_transport(std::int32_t y)
{
    s_row_y  = y;
    s_row[0] = button(s_view, LV_SYMBOL_PREV, EPISODE_W, TRANSPORT_H,
                      [](lv_event_t *) { media_action(MediaAction::Previous); });
    s_row[1] = button(s_view, "", STEP_W, TRANSPORT_H,
                      [](lv_event_t *) { media_seek_by(-SEEK_STEP_S); });
    mark(s_row[1], &icons::seek_back_icon);
    s_play = button(s_view, LV_SYMBOL_PLAY, PLAY_W, TRANSPORT_H,
                    [](lv_event_t *) { media_toggle_play(); });
    theme::fill_accent(s_play);
    s_row[2] = s_play;
    s_row[3] = button(s_view, "", STEP_W, TRANSPORT_H,
                      [](lv_event_t *) { media_seek_by(SEEK_STEP_S); });
    mark(s_row[3], &icons::seek_on_icon);
    s_row[4] = button(s_view, LV_SYMBOL_NEXT, EPISODE_W, TRANSPORT_H,
                      [](lv_event_t *) { media_action(MediaAction::Next); });
    place_row();
    build_volume(y);
}

void preset(int index)
{
    if (s_handlers.preset != nullptr) {
        s_handlers.preset(index, false);
    }
}

void build_room(std::int32_t y)
{
    s_low = button(s_view, preset_name(ULTRA_LOW_PRESET), ROOM_W, ROOM_H,
                   [](lv_event_t *) { preset(ULTRA_LOW_PRESET); }, fonts::size_28());
    lv_obj_set_pos(s_low, PAD, y);
    s_lights = button(s_view, "Lights", ROOM_W, ROOM_H,
                      [](lv_event_t *) {
                          if (s_handlers.lights != nullptr) {
                              s_handlers.lights();
                          }
                      },
                      fonts::size_28());
    lv_obj_set_pos(s_lights, PAD + ROOM_W + BUTTON_GAP, y);
    // The screen going dark by itself, lit while it does.
    s_screen = button(s_view, "", ROOM_H, ROOM_H, [](lv_event_t *) { s_auto_off = !s_auto_off; });
    mark(s_screen, &icons::screen_timer_icon);
    lv_obj_set_pos(s_screen, PAD + 2 * (ROOM_W + BUTTON_GAP), y);
    s_subtitles = button(s_view, "", ROOM_H, ROOM_H, [](lv_event_t *) {
        s_subtitles_shown = !s_subtitles_shown;  // the next report from the player confirms it
        media_action(MediaAction::Subtitles);
        tick(s_tick);
    });
    mark(s_subtitles, &icons::subtitles_icon);
    lv_obj_set_pos(s_subtitles, PAD + 2 * (ROOM_W + BUTTON_GAP) + ROOM_H + BUTTON_GAP, y);
}
}  // namespace

void build_cinema(lv_obj_t *screen)
{
    const Layout l = layout();
    s_view = lv_obj_create(screen);
    lv_obj_set_size(s_view, l.screen_w, l.screen_h);
    lv_obj_set_pos(s_view, 0, 0);
    theme::style_panel(s_view, theme::background, 0);
    lv_obj_set_hidden(s_view, true);

    build_film(l.screen_w);
    const std::int32_t room_y      = l.screen_h - PAD - ROOM_H;
    const std::int32_t transport_y = room_y - BUTTON_GAP * 2 - TRANSPORT_H;
    build_transport(transport_y);
    build_room(room_y);

    s_tick = lv_timer_create(tick, TICK_MS, nullptr);
    lv_timer_pause(s_tick);
    s_cinema = add_view({"cinema", ViewKind::Fullscreen, s_view,
                         [] {
                             s_woke_at          = lv_tick_get();
                             s_skip_was_offered = false;
                             lv_timer_resume(s_tick);
                             tick(s_tick);
                             update_view_clocks();
                         },
                         [] { lv_timer_pause(s_tick); }});
    add_fullscreen_chrome(s_cinema, s_view, [](lv_event_t *) { close_cinema(); });
}

bool cinema_has_next()
{
    return s_neighbour[1];
}

void apply_media_neighbours(bool previous, bool next)
{
    s_neighbour[0] = previous;
    s_neighbour[1] = next;
    if (s_row[0] != nullptr) {
        place_row();
    }
}

void apply_media_subtitles(bool available, bool shown)
{
    s_subtitles_available = available;
    s_subtitles_shown     = shown;
}

void apply_cinema_still(const void *pixels)
{
    if (s_still == nullptr) {
        return;
    }
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

bool cinema_open()
{
    return view_open(s_cinema);
}

void open_cinema()
{
    open_view(s_cinema);
}
}  // namespace ui::detail
