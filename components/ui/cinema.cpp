#include "ui_internal.h"

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
constexpr std::int32_t TOP_Y         = 104;  // under the eyebrow and the close button
constexpr std::int32_t CLOSE         = 64;
constexpr std::int32_t PROGRESS_H    = 10;
constexpr std::int32_t LINE_GAP      = 12;
constexpr std::int32_t TRANSPORT_H   = 96;
constexpr std::int32_t STEP_W        = 150;
constexpr std::int32_t PLAY_W        = 220;
constexpr std::int32_t SKIP_W        = 240;
constexpr std::int32_t ROOM_W        = 220;  // the desk and light buttons
constexpr std::int32_t ROOM_H        = 84;
constexpr int          SEEK_STEP_S   = 10;
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
lv_obj_t     *s_lights  = nullptr;
lv_obj_t     *s_sit     = nullptr;
lv_obj_t     *s_low     = nullptr;
lv_timer_t   *s_tick    = nullptr;
std::uint32_t s_woke_at = 0;  // lit for an intro to skip: kept lit a while from then
bool          s_skip_was_offered = false;

void close_cinema()
{
    lv_obj_set_hidden(s_view, true);
    lv_timer_pause(s_tick);
}

/** The artist line holds the series, then its season and episode. */
void show_text()
{
    const char *title  = lv_label_get_text(s_media_title);
    const char *artist = lv_label_get_text(s_media_artist);
    const char *split  = std::strchr(artist, '\n');
    theme::set_text(s_title, title);
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
    if (s_screen_on && untouched && !offered) {
        set_screen_state(false);
    }
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
    const char *skip = media_skip_text();
    lv_obj_set_hidden(s_skip, skip == nullptr);
    if (skip != nullptr) {
        theme::set_text(lv_obj_get_child(s_skip, 0), skip);
    }
    theme::fill_accent_or(s_lights, s_lights_on, theme::panel_light);
    theme::fill_accent_or(s_sit, s_preset_active[SIT_PRESET], theme::panel_light);
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

    const std::int32_t x = PAD + STILL_W + TEXT_GAP;
    const std::int32_t w = width - x - PAD;
    std::int32_t       y = TOP_Y;
    s_series = line(s_view, theme::secondary, fonts::size_22(), w, 1);
    lv_obj_set_pos(s_series, x, y);
    y += lv_font_get_line_height(fonts::size_22()) + LINE_GAP;
    s_title = line(s_view, theme::text, fonts::size_48(), w, 2);
    lv_obj_set_pos(s_title, x, y);
    y += 2 * lv_font_get_line_height(fonts::size_48()) + LINE_GAP;
    s_episode = line(s_view, theme::secondary, fonts::size_22(), w, 1);
    lv_obj_set_pos(s_episode, x, y);

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
}

void build_transport(std::int32_t y)
{
    std::int32_t x = PAD;
    lv_obj_t *back = button(s_view, "", STEP_W, TRANSPORT_H,
                            [](lv_event_t *) { media_seek_by(-SEEK_STEP_S); });
    mark(back, &icons::seek_back_icon);
    lv_obj_set_pos(back, x, y);
    x += STEP_W + BUTTON_GAP;
    s_play = button(s_view, LV_SYMBOL_PLAY, PLAY_W, TRANSPORT_H,
                    [](lv_event_t *) { media_toggle_play(); });
    theme::fill_accent(s_play);
    lv_obj_set_pos(s_play, x, y);
    x += PLAY_W + BUTTON_GAP;
    lv_obj_t *on = button(s_view, "", STEP_W, TRANSPORT_H,
                          [](lv_event_t *) { media_seek_by(SEEK_STEP_S); });
    mark(on, &icons::seek_on_icon);
    lv_obj_set_pos(on, x, y);
    x += STEP_W + BUTTON_GAP;
    s_skip = button(s_view, "Skip intro", SKIP_W, TRANSPORT_H, [](lv_event_t *) { media_skip(); },
                    fonts::size_28());
    theme::fill_accent(s_skip);
    lv_obj_set_pos(s_skip, x, y);
    lv_obj_set_hidden(s_skip, true);
}

void preset(int index)
{
    if (s_handlers.preset != nullptr) {
        s_handlers.preset(index, false);
    }
}

void build_room(std::int32_t y)
{
    s_sit = button(s_view, preset_name(SIT_PRESET), ROOM_W, ROOM_H,
                   [](lv_event_t *) { preset(SIT_PRESET); }, fonts::size_28());
    lv_obj_set_pos(s_sit, PAD, y);
    s_low = button(s_view, preset_name(ULTRA_LOW_PRESET), ROOM_W, ROOM_H,
                   [](lv_event_t *) { preset(ULTRA_LOW_PRESET); }, fonts::size_28());
    lv_obj_set_pos(s_low, PAD + ROOM_W + BUTTON_GAP, y);
    s_lights = button(s_view, "Lights", ROOM_W, ROOM_H,
                      [](lv_event_t *) {
                          if (s_handlers.lights != nullptr) {
                              s_handlers.lights();
                          }
                      },
                      fonts::size_28());
    lv_obj_set_pos(s_lights, PAD + 2 * (ROOM_W + BUTTON_GAP), y);
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

    lv_obj_t *eyebrow = theme::make_accent_label(s_view, "CINEMA", fonts::size_22());
    lv_obj_set_pos(eyebrow, PAD, PAD);
    lv_obj_t *close = button(s_view, LV_SYMBOL_CLOSE, CLOSE, CLOSE, [](lv_event_t *) { close_cinema(); },
                             fonts::size_28());
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -PAD, PAD - LINE_GAP);

    build_film(l.screen_w);
    const std::int32_t room_y      = l.screen_h - PAD - ROOM_H;
    const std::int32_t transport_y = room_y - BUTTON_GAP * 2 - TRANSPORT_H;
    build_transport(transport_y);
    build_room(room_y);

    s_tick = lv_timer_create(tick, TICK_MS, nullptr);
    lv_timer_pause(s_tick);
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

void open_cinema()
{
    if (s_view == nullptr) {
        return;
    }
    lv_obj_set_hidden(s_view, false);
    lv_obj_move_foreground(s_view);
    s_woke_at          = lv_tick_get();
    s_skip_was_offered = false;
    lv_timer_resume(s_tick);
    tick(s_tick);
}
}  // namespace ui::detail
