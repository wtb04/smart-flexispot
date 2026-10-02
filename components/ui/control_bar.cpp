#include "ui_internal.h"

#include "home_model.h"
#include "media_model.h"
#include "popout.h"
#include "room_model.h"
#include "topics.h"

#include <cstdio>
#include <cstring>
#include <utility>

// The control bar's slots for what is touched every day: what plays, with play
// and pause in it; the heating, which drops its dial; and the lights, all of
// them with a tap and each when held. Each has its width and its place for
// good, and says so in the same place when there is nothing to say.
namespace ui::detail {
namespace {
constexpr std::int32_t BTN_H        = 56;  // a button inside a slot
constexpr std::int32_t SLOT_PAD     = 10;
constexpr std::int32_t SLOT_TAP     = 6;   // half the gap to the next slot, which takes the rest
// With the timer and the status, as wide as the bar is with the battery and an
// update both showing, so nothing ever runs off its end.
constexpr std::int32_t MEDIA_W      = 440;
constexpr std::int32_t HEAT_W       = 140;
constexpr std::int32_t LIGHTS_W     = 120;
constexpr std::uint32_t SKIP_CHECK_MS = 500;
constexpr std::int32_t THUMB        = 52;
constexpr std::int32_t THUMB_RADIUS = 6;   // small enough to leave a narrow poster its corners
constexpr std::int32_t PLAY_D       = 48;
constexpr std::int32_t TEXT_GAP     = 12;
constexpr std::int32_t HEAT_DOT     = 12;

constexpr std::uint32_t CARD_IDLE_MS = 20 * 1000;
constexpr std::int32_t  CARD_PAD     = 24;
constexpr std::int32_t  CARD_GAP     = 12;

constexpr std::int32_t  MEDIA_CARD_W = 480;
constexpr std::int32_t  COVER        = 140;
constexpr std::int32_t  CARD_BAR_H   = 6;
constexpr std::int32_t  STEP_W       = 88;
constexpr std::uint32_t GLIDE_MS     = 250;
constexpr std::int32_t  CARD_BTN_H   = 64;
constexpr std::int32_t  VOLUME_INSET = 18;
constexpr int           SEEK_STEP_S  = 10;

constexpr std::int32_t DIAL_W = 480;  // the thermostat as Home had it
constexpr std::int32_t DIAL_H = 520;

constexpr std::int32_t LIGHTS_CARD_W = 380;
constexpr std::int32_t LIGHT_BTN_H   = 72;

lv_obj_t *make_slot(lv_obj_t *bar, std::int32_t w)
{
    lv_obj_t *slot = lv_button_create(bar);
    theme::style_button(slot, theme::panel);
    lv_obj_set_size(slot, w, TOP_H);
    lv_obj_set_style_radius(slot, theme::radius::pill, 0);
    lv_obj_set_style_pad_hor(slot, SLOT_PAD, 0);
    lv_obj_set_ext_click_area(slot, SLOT_TAP);
    lv_obj_set_scrollable(slot, false);
    // Lighter while pressed, or while its card is out.
    for (const lv_state_t state : {LV_STATE_PRESSED, LV_STATE_CHECKED}) {
        lv_obj_set_style_bg_color(slot, lv_color_hex(theme::panel_light), state);
    }
    return slot;
}

lv_obj_t *one_line(lv_obj_t *parent, std::uint32_t ink, const lv_font_t *font, std::int32_t w)
{
    lv_obj_t *label = theme::make_label(parent, "", ink, font);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_clickable(label, false);
    return label;
}

// Under the slot, its edge level with the slot's on the page's side, unfolding
// away from the dock as the timer's card does towards it.
void open_under(Popout &p, lv_obj_t *slot)
{
    if (!p.open) {
        lv_area_t at;
        lv_obj_get_coords(slot, &at);
        const bool right = layout().rail_right;
        place_popout(p, right ? at.x1 - GAP : at.x2 + 1 + GAP, at.y2 + 1, !right);
    }
    open_popout(p, !p.open);
}

// A cover to show at any size: its own descriptor, pointed at the model's
// pixels, scaled into the frame it is in.
struct Cover {
    std::int32_t   side  = 0;        // its height, and its width for a square cover
    lv_obj_t      *frame = nullptr;
    lv_obj_t      *image = nullptr;
    lv_obj_t      *mark  = nullptr;  // shown while there is none
    lv_image_dsc_t dsc[2]{};         // in turn, so the one on screen is never rewritten under it
    int            slot   = 0;
    std::uint32_t  covers = UINT32_MAX;
};

void build_cover(Cover &cover, lv_obj_t *parent, std::int32_t side, std::int32_t radius)
{
    cover.side  = side;
    cover.frame = lv_obj_create(parent);
    theme::style_panel(cover.frame, theme::panel_light, radius);
    lv_obj_set_size(cover.frame, side, side);
    lv_obj_set_style_clip_corner(cover.frame, true, 0);
    lv_obj_set_scrollable(cover.frame, false);
    lv_obj_set_clickable(cover.frame, false);
    cover.image = lv_image_create(cover.frame);
    lv_obj_set_size(cover.image, side, side);
    lv_image_set_inner_align(cover.image, LV_IMAGE_ALIGN_COVER);
    lv_obj_center(cover.image);
    lv_obj_set_clickable(cover.image, false);
    lv_obj_set_hidden(cover.image, true);
    if (side >= COVER) {
        // Large enough for the speaker itself, as Home's card showed it.
        cover.mark = lv_image_create(cover.frame);
        lv_image_set_src(cover.mark, speaker_picture(side));
    } else {
        cover.mark = theme::make_label(cover.frame, LV_SYMBOL_AUDIO, theme::secondary, fonts::size_22());
    }
    lv_obj_center(cover.mark);
    lv_obj_set_clickable(cover.mark, false);
}

void paint_cover(Cover &cover)
{
    const MediaState &media = media_state();
    const bool        shown = media.has_track && media.art != nullptr;
    lv_obj_set_hidden(cover.image, !shown);
    lv_obj_set_hidden(cover.mark, shown);
    // A poster is kept whole, as narrow as it is, rather than cut to a square.
    const std::int32_t w = shown ? std::max<std::int32_t>(1, cover.side * media.art_width / media::kArtSize) : cover.side;
    if (lv_obj_get_width(cover.frame) != w) {
        lv_obj_set_width(cover.frame, w);
        lv_obj_set_size(cover.image, w, cover.side);
        lv_obj_center(cover.image);
    }
    if (!shown || media.covers == cover.covers) {
        return;
    }
    cover.covers           = media.covers;
    lv_image_dsc_t &dsc    = cover.dsc[cover.slot];
    cover.slot             = 1 - cover.slot;
    const std::uint32_t px = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    dsc.header.magic       = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf          = LV_COLOR_FORMAT_RGB565;
    dsc.header.w           = static_cast<std::uint32_t>(media.art_width);
    dsc.header.h           = media::kArtSize;
    dsc.header.stride      = static_cast<std::uint32_t>(media.art_width) * px;
    dsc.data_size          = dsc.header.stride * media::kArtSize;
    dsc.data               = static_cast<const std::uint8_t *>(media.art);
    lv_image_set_src(cover.image, &dsc);
}

// A video's artist is its series, a newline, and the episode: the series is enough here.
void first_line(const char *text, char *out, std::size_t size)
{
    const char *end = std::strchr(text, '\n');
    const int   n   = end != nullptr ? static_cast<int>(end - text) : static_cast<int>(std::strlen(text));
    std::snprintf(out, size, "%.*s", n, text);
}

// ---- What plays ----

lv_obj_t *s_media_slot  = nullptr;
Cover     s_slot_cover;
lv_obj_t *s_slot_title  = nullptr;
lv_obj_t *s_slot_artist = nullptr;
lv_obj_t *s_slot_play   = nullptr;
lv_obj_t *s_slot_skip   = nullptr;  // Skip intro, or Next episode, over the text while it is offered
bool      s_media_held  = false;    // a hold fires LONG_PRESSED and then CLICKED on release

Popout    s_media_pop;
Cover     s_card_cover;
lv_obj_t *s_card_source  = nullptr;
lv_obj_t *s_card_title   = nullptr;
lv_obj_t *s_card_artist  = nullptr;
lv_obj_t *s_card_episode = nullptr;  // a video's season and episode, under its series
lv_obj_t *s_card_full    = nullptr;
lv_obj_t *s_card_bar     = nullptr;  // how far it has got
lv_obj_t *s_card_at      = nullptr;
lv_obj_t *s_card_length  = nullptr;
lv_obj_t *s_card_prev    = nullptr;
lv_obj_t *s_card_play    = nullptr;
lv_obj_t *s_card_next    = nullptr;
lv_obj_t *s_volume       = nullptr;
lv_obj_t *s_volume_fill  = nullptr;
lv_obj_t *s_volume_text  = nullptr;
bool      s_volume_held  = false;
int       s_card_at_s    = -1;

void show_play(lv_obj_t *button, bool playing)
{
    lv_obj_set_state(button, LV_STATE_CHECKED, playing);
    theme::set_text(lv_obj_get_child(button, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void show_volume(int percent)
{
    lv_obj_set_width(s_volume_fill, lv_obj_get_width(s_volume) * percent / 100);
    char text[12];
    std::snprintf(text, sizeof(text), "%d", percent);
    theme::set_text(s_volume_text, text);
}

// How far it has got, as the music view shows it, while the card is out.
void glide_card(lv_timer_t *)
{
    const MediaState &media = media_state();
    if (!s_media_pop.open || media.duration_s <= 0 || !media.has_track) {
        return;
    }
    const int at_ms = media_position_ms_now();
    lv_bar_set_value(s_card_bar, at_ms / units::kMsPerSecond, LV_ANIM_OFF);
    if (at_ms / units::kMsPerSecond != s_card_at_s) {
        s_card_at_s = at_ms / units::kMsPerSecond;
        write_clock(s_card_at, s_card_at_s);
    }
}

void paint_media()
{
    const MediaState &media = media_state();
    char              artist[sizeof(media.artist)];
    first_line(media.artist, artist, sizeof(artist));
    theme::set_text(s_slot_title, media.has_track ? media.title : "Nothing playing");
    theme::set_text_color(s_slot_title, media.has_track ? theme::text : theme::secondary);
    theme::set_text(s_slot_artist, media.has_track ? artist : "");
    show_play(s_slot_play, media.has_track && media.playing);
    // With nothing playing, play offers the favourites.
    theme::set_usable(s_slot_play, !media.has_track || media.remote);
    paint_cover(s_slot_cover);

    if (s_card_title == nullptr) {
        return;
    }
    theme::set_text(s_card_source, media.source);
    theme::set_text(s_card_title, media.has_track ? media.title : "Nothing playing");
    // One line or two, as the title needs, so who it is by follows it closely.
    const std::int32_t line = lv_font_get_line_height(fonts::size_28());
    lv_point_t         size{};
    lv_obj_update_layout(s_card_title);
    lv_text_get_size(&size, lv_label_get_text(s_card_title), fonts::size_28(), 0, 0, lv_obj_get_width(s_card_title),
                     LV_TEXT_FLAG_NONE);
    lv_obj_set_height(s_card_title, size.y > line ? 2 * line : line);
    theme::set_text(s_card_artist, media.has_track ? artist : "");
    const char *episode = std::strchr(media.artist, '\n');
    theme::set_text(s_card_episode, media.has_track && episode != nullptr ? episode + 1 : "");
    theme::set_usable(s_card_full, media.has_track);
    theme::set_text(lv_obj_get_child(s_card_prev, 0), media.video ? "-10 s" : LV_SYMBOL_PREV);
    theme::set_text(lv_obj_get_child(s_card_next, 0), media.video ? "+10 s" : LV_SYMBOL_NEXT);
    show_play(s_card_play, media.has_track && media.playing);
    for (lv_obj_t *button : {s_card_prev, s_card_play, s_card_next}) {
        theme::set_usable(button, media.has_track && media.remote);
    }
    const bool timed = media.has_track && media.duration_s > 0;
    for (lv_obj_t *part : {s_card_bar, s_card_at, s_card_length}) {
        lv_obj_set_style_opa(part, timed ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    if (timed) {
        lv_bar_set_range(s_card_bar, 0, media.duration_s);
        write_clock(s_card_length, media.duration_s);
        s_card_at_s = -1;
        glide_card(nullptr);
    }
    theme::set_usable(s_volume, media.volume >= 0);
    if (media.volume >= 0 && !s_volume_held) {
        show_volume(media.volume);
    }
    paint_cover(s_card_cover);
}

void open_full_view()
{
    if (!media_state().has_track) {
        return;
    }
    open_popout(s_media_pop, false);
    media_is_video() ? open_cinema() : open_music();
}

// Its card on a tap. Held: the favourites while nothing plays, or the desk to
// the preset set for what plays, as the cinema view's low desk.
void media_touched(lv_event_t *e)
{
    const MediaState &media = media_state();
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        s_media_held = true;
        if (!media.has_track) {
            open_favourites();
        } else if (media.hold_preset >= 0) {
            desk_go_to(media.hold_preset);
        } else {
            open_full_view();
        }
        return;
    }
    if (!std::exchange(s_media_held, false)) {
        open_under(s_media_pop, s_media_slot);
    }
}

void skip_check(lv_timer_t *)
{
    const MediaSkip offer = media_skip_offer();
    lv_obj_set_hidden(s_slot_skip, offer.text == nullptr);
    for (lv_obj_t *line : {s_slot_title, s_slot_artist}) {
        lv_obj_set_hidden(line, offer.text != nullptr);
    }
    if (offer.text != nullptr) {
        theme::set_text(lv_obj_get_child(s_slot_skip, 0), offer.text);
    }
}

void play_clicked(lv_event_t *)
{
    if (media_state().has_track) {
        media_toggle_play();
    } else {
        open_favourites();
    }
}

void step_clicked(lv_event_t *e)
{
    const bool next = lv_event_get_user_data(e) != nullptr;
    if (media_state().video) {
        media_seek_by(next ? SEEK_STEP_S : -SEEK_STEP_S);
    } else {
        media_action(next ? MediaAction::Next : MediaAction::Previous);
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
    if (percent != media_state().volume) {
        media_set_volume(percent);
        show_volume(percent);
    }
}

lv_obj_t *card_button(lv_obj_t *parent, const char *text, std::int32_t w)
{
    lv_obj_t *button = theme::make_button(parent, text, theme::panel_light, fonts::size_22());
    lv_obj_set_size(button, w, CARD_BTN_H);
    return button;
}

// A small round button, its symbol at the bar's text size.
lv_obj_t *round_button(lv_obj_t *parent, const char *symbol, std::int32_t side)
{
    lv_obj_t *button = theme::make_button(parent, symbol, theme::panel_light, fonts::size_22());
    lv_obj_set_size(button, side, side);
    lv_obj_set_style_radius(button, side / 2, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(theme::panel_light), LV_STATE_CHECKED);  // the symbol says it
    return button;
}

// The cover, what plays and who by, and play: a small player, the rest in its card.
void build_media_slot(lv_obj_t *bar)
{
    s_media_slot = make_slot(bar, MEDIA_W);
    lv_obj_add_event_cb(s_media_slot, media_touched, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_media_slot, media_touched, LV_EVENT_LONG_PRESSED, nullptr);
    build_cover(s_slot_cover, s_media_slot, THUMB, THUMB_RADIUS);
    lv_obj_align(s_slot_cover.frame, LV_ALIGN_LEFT_MID, 0, 0);

    const std::int32_t text_x = THUMB + TEXT_GAP;
    const std::int32_t text_w = MEDIA_W - 2 * SLOT_PAD - text_x - PLAY_D - TEXT_GAP;
    s_slot_title              = one_line(s_media_slot, theme::text, fonts::size_20(), text_w);
    lv_obj_align(s_slot_title, LV_ALIGN_LEFT_MID, text_x, -11);
    s_slot_artist = one_line(s_media_slot, theme::secondary, fonts::size_16(), text_w);
    lv_obj_align(s_slot_artist, LV_ALIGN_LEFT_MID, text_x, 13);

    s_slot_play = round_button(s_media_slot, LV_SYMBOL_PLAY, PLAY_D);
    lv_obj_align(s_slot_play, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_slot_play, play_clicked, LV_EVENT_CLICKED, nullptr);

    // Where the text is, while an intro or the credits run: one tap, as Home's card had it.
    s_slot_skip = theme::make_button(s_media_slot, "Skip intro", theme::panel_light, theme::type_body());
    theme::fill_accent(s_slot_skip);
    lv_obj_set_size(s_slot_skip, text_w, PLAY_D);
    lv_obj_set_style_radius(s_slot_skip, PLAY_D / 2, 0);
    lv_obj_align(s_slot_skip, LV_ALIGN_LEFT_MID, text_x, 0);
    lv_obj_add_event_cb(s_slot_skip, [](lv_event_t *) {
        media_skip();
        skip_check(nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_slot_skip, true);
    lv_timer_create(skip_check, SKIP_CHECK_MS, nullptr);
}

lv_obj_t *row_of(lv_obj_t *card, std::int32_t w, std::int32_t h, lv_flex_align_t main)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, CARD_GAP, 0);
    lv_obj_set_clickable(row, false);
    return row;
}

// The cover large, what plays, how far it has got, and its controls, as a
// small music view: the fullscreen one is a tap on the corner away.
void build_media_card(lv_obj_t *screen)
{
    lv_obj_t *card     = build_popout(s_media_pop, screen, MEDIA_CARD_W, GAP, GAP, CARD_IDLE_MS);
    s_media_pop.button = s_media_slot;
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, CARD_GAP, 0);
    const std::int32_t inner = MEDIA_CARD_W - 2 * CARD_PAD;

    lv_obj_t *head = row_of(card, inner, COVER, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    build_cover(s_card_cover, head, COVER, theme::radius::control);
    lv_obj_set_clickable(s_card_cover.frame, true);
    lv_obj_add_event_cb(s_card_cover.frame, [](lv_event_t *) { open_full_view(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *text = lv_obj_create(head);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, COVER);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text, theme::space::xs, 0);
    lv_obj_set_clickable(text, false);
    s_card_source  = one_line(text, theme::secondary, theme::type_label(), 1);
    s_card_title   = theme::make_label(text, "", theme::text, fonts::size_28());
    lv_label_set_long_mode(s_card_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_card_title, 2 * lv_font_get_line_height(fonts::size_28()));
    s_card_artist  = one_line(text, theme::secondary, fonts::size_20(), 1);
    s_card_episode = one_line(text, theme::secondary, fonts::size_16(), 1);
    for (lv_obj_t *line : {s_card_source, s_card_title, s_card_artist, s_card_episode}) {
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_clickable(line, false);
    }
    s_card_full = theme::make_chip(head, "");
    theme::make_mark(s_card_full, &icons::expand_icon);
    lv_obj_set_style_bg_color(s_card_full, lv_color_hex(theme::panel_light), 0);
    lv_obj_add_event_cb(s_card_full, [](lv_event_t *) { open_full_view(); }, LV_EVENT_CLICKED, nullptr);

    s_card_bar = lv_bar_create(card);
    lv_obj_set_size(s_card_bar, inner, CARD_BAR_H);
    theme::style_panel(s_card_bar, theme::panel_light, CARD_BAR_H / 2);
    theme::fill_accent(s_card_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_card_bar, CARD_BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_t *times = row_of(card, inner, lv_font_get_line_height(fonts::size_16()), LV_FLEX_ALIGN_SPACE_BETWEEN);
    lv_obj_set_style_margin_top(times, -CARD_GAP / 2, 0);
    s_card_at     = theme::make_label(times, "", theme::secondary, fonts::size_16());
    s_card_length = theme::make_label(times, "", theme::secondary, fonts::size_16());

    lv_obj_t *transport = row_of(card, inner, CARD_BTN_H, LV_FLEX_ALIGN_CENTER);
    s_card_prev = card_button(transport, LV_SYMBOL_PREV, STEP_W);
    lv_obj_add_event_cb(s_card_prev, step_clicked, LV_EVENT_CLICKED, nullptr);
    s_card_play = card_button(transport, LV_SYMBOL_PLAY, inner - 2 * (STEP_W + CARD_GAP));
    theme::fill_accent(s_card_play, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_card_play, [](lv_event_t *) { media_toggle_play(); }, LV_EVENT_CLICKED, nullptr);
    s_card_next = card_button(transport, LV_SYMBOL_NEXT, STEP_W);
    lv_obj_add_event_cb(s_card_next, step_clicked, LV_EVENT_CLICKED, s_card_next);

    // The volume, as the music view has it, the favourites beside it.
    lv_obj_t *sound = row_of(card, inner, CARD_BTN_H, LV_FLEX_ALIGN_START);
    s_volume = lv_obj_create(sound);
    theme::style_panel(s_volume, theme::panel_light, theme::radius::control);
    lv_obj_set_height(s_volume, CARD_BTN_H);
    lv_obj_set_flex_grow(s_volume, 1);
    lv_obj_set_style_clip_corner(s_volume, true, 0);
    lv_obj_set_scrollable(s_volume, false);
    lv_obj_set_gesture_bubble(s_volume, false);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(s_volume, volume_touched, code, nullptr);
    }
    s_volume_fill = lv_obj_create(s_volume);
    theme::style_panel(s_volume_fill, theme::secondary, 0);
    lv_obj_set_style_bg_opa(s_volume_fill, LV_OPA_40, 0);
    lv_obj_set_size(s_volume_fill, 0, CARD_BTN_H);
    lv_obj_set_clickable(s_volume_fill, false);
    lv_obj_t *speaker = theme::make_label(s_volume, LV_SYMBOL_VOLUME_MAX, theme::text, fonts::size_22());
    lv_obj_align(speaker, LV_ALIGN_LEFT_MID, VOLUME_INSET, 0);
    lv_obj_set_clickable(speaker, false);
    s_volume_text = theme::make_label(s_volume, "", theme::text, fonts::size_22());
    lv_obj_align(s_volume_text, LV_ALIGN_RIGHT_MID, -VOLUME_INSET, 0);
    lv_obj_set_clickable(s_volume_text, false);
    lv_obj_t *favourites = card_button(sound, LV_SYMBOL_LIST, STEP_W);
    lv_obj_add_event_cb(favourites, [](lv_event_t *) {
        open_popout(s_media_pop, false);
        open_favourites();
    }, LV_EVENT_CLICKED, nullptr);
    lv_timer_create(glide_card, GLIDE_MS, nullptr);
    fit_popout(s_media_pop);
}

// ---- The heating ----

lv_obj_t *s_heat_slot   = nullptr;
lv_obj_t *s_heat_dot    = nullptr;
lv_obj_t *s_heat_now    = nullptr;
Popout    s_heat_pop;

void degrees(lv_obj_t *label, const char *prefix, float celsius)
{
    char text[24] = "--";
    if (celsius >= 0.0f) {
        const int tenths = static_cast<int>(celsius * 10.0f + 0.5f);
        std::snprintf(text, sizeof(text), "%s%d.%d°", prefix, tenths / 10, tenths % 10);
    }
    theme::set_text(label, text);
}

void paint_heat()
{
    const ThermostatState &t = home_state().thermostat;
    // The room only; its dot says heating, warm enough or off, the target is the dial's.
    degrees(s_heat_now, "", t.known ? t.current_c : -1.0f);
    const bool          off = !t.known || t.state == Hvac::Off;
    const std::uint32_t ink = t.state == Hvac::Heating ? theme::primary : t.state == Hvac::Idle ? theme::amber : theme::secondary;
    theme::set_bg_color(s_heat_dot, off ? theme::secondary : ink);
}

void build_heat_slot(lv_obj_t *bar)
{
    s_heat_slot = make_slot(bar, HEAT_W);
    lv_obj_set_flex_flow(s_heat_slot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_heat_slot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_heat_slot, TEXT_GAP, 0);
    lv_obj_add_event_cb(s_heat_slot, [](lv_event_t *) { open_under(s_heat_pop, s_heat_slot); }, LV_EVENT_CLICKED,
                        nullptr);
    s_heat_dot = lv_obj_create(s_heat_slot);
    theme::style_panel(s_heat_dot, theme::secondary, HEAT_DOT / 2);
    lv_obj_set_size(s_heat_dot, HEAT_DOT, HEAT_DOT);
    lv_obj_set_clickable(s_heat_dot, false);
    s_heat_now = theme::make_label(s_heat_slot, "--", theme::text, fonts::size_28());
    lv_obj_set_clickable(s_heat_now, false);
}

void build_heat_card(lv_obj_t *screen)
{
    lv_obj_t *card    = build_popout(s_heat_pop, screen, DIAL_W, GAP, GAP, CARD_IDLE_MS);
    s_heat_pop.button = s_heat_slot;
    lv_obj_set_height(card, DIAL_H);
    build_thermostat_card(card, DIAL_W, DIAL_H);
    fit_popout(s_heat_pop);
}

// ---- The lights ----

lv_obj_t *s_lights_slot = nullptr;
lv_obj_t *s_lights_bulb = nullptr;
lv_obj_t *s_lights_text = nullptr;
bool      s_lights_held = false;  // a hold fires LONG_PRESSED and then CLICKED on release
Popout    s_lights_pop;
lv_obj_t *s_light_buttons[kLightCount] = {};

void paint_lights_slot()
{
    const LightsState &lights = lights_state();
    int                on     = 0;
    for (const bool lit : lights.light_on) {
        on += lit ? 1 : 0;
    }
    char text[16] = "";
    if (lights.on && on > 0) {
        std::snprintf(text, sizeof(text), "%d", on);
    }
    // On in the accent, rather than the whole slot filled with it.
    theme::set_text(s_lights_text, text);
    theme::set_text_color(s_lights_text, lights.on ? theme::primary : theme::secondary);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(s_lights_bulb); ++i) {
        lv_obj_set_style_image_recolor(lv_obj_get_child(s_lights_bulb, static_cast<std::int32_t>(i)),
                                       lv_color_hex(lights.on ? theme::primary : theme::secondary), 0);
    }

    for (int i = 0; i < kLightCount; ++i) {
        lv_obj_t        *button = s_light_buttons[i];
        const LightState &light = lights.lights[i];
        if (button == nullptr) {
            continue;
        }
        lv_obj_set_hidden(button, light.name[0] == '\0');
        lv_obj_set_state(button, LV_STATE_CHECKED, lights.light_on[i]);
        theme::set_text(lv_obj_get_child(button, 0), light.name);
        theme::set_text(lv_obj_get_child(button, 1), light.state[0] != '\0' ? light.state : "--");
    }
    if (s_lights_pop.card != nullptr) {
        fit_popout(s_lights_pop);
    }
}

void lights_touched(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        s_lights_held = true;
        open_under(s_lights_pop, s_lights_slot);
        return;
    }
    if (std::exchange(s_lights_held, false)) {
        return;
    }
    if (s_handlers.lights != nullptr) {
        s_handlers.lights();
    }
}

void build_lights_slot(lv_obj_t *bar)
{
    s_lights_slot = make_slot(bar, LIGHTS_W);
    lv_obj_set_flex_flow(s_lights_slot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_lights_slot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_lights_slot, TEXT_GAP, 0);
    lv_obj_add_event_cb(s_lights_slot, lights_touched, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_lights_slot, lights_touched, LV_EVENT_LONG_PRESSED, nullptr);
    // The whole bulb, the glass over its base, as Home's lights were drawn.
    s_lights_bulb = lv_obj_create(s_lights_slot);
    lv_obj_remove_style_all(s_lights_bulb);
    lv_obj_set_size(s_lights_bulb, icons::bulb_glass_icon.header.w, icons::bulb_glass_icon.header.h);
    lv_obj_set_clickable(s_lights_bulb, false);
    for (const lv_image_dsc_t *part : {&icons::bulb_glass_icon, &icons::bulb_base_icon}) {
        lv_obj_t *image = lv_image_create(s_lights_bulb);
        lv_image_set_src(image, part);
        lv_obj_set_pos(image, 0, 0);
        lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
        lv_obj_set_clickable(image, false);
    }
    s_lights_text = theme::make_label(s_lights_slot, "", theme::secondary, fonts::size_22());
    lv_obj_set_clickable(s_lights_text, false);
}

void build_lights_card(lv_obj_t *screen)
{
    lv_obj_t *card      = build_popout(s_lights_pop, screen, LIGHTS_CARD_W, GAP, GAP, CARD_IDLE_MS);
    s_lights_pop.button = s_lights_slot;
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, CARD_GAP, 0);
    const std::int32_t inner = LIGHTS_CARD_W - 2 * CARD_PAD;
    for (int i = 0; i < kLightCount; ++i) {
        lv_obj_t *button = lv_button_create(card);
        theme::style_button(button, theme::panel_light);
        theme::fill_accent(button, LV_STATE_CHECKED);
        lv_obj_set_size(button, inner, LIGHT_BTN_H);
        lv_obj_set_style_pad_hor(button, CARD_PAD, 0);
        lv_obj_add_event_cb(button, [](lv_event_t *e) {
            if (s_handlers.light != nullptr) {
                s_handlers.light(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
            }
        }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        lv_obj_t *name = one_line(button, theme::text, fonts::size_22(), inner * 2 / 3);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *state = theme::make_label(button, "", theme::text, fonts::size_20());
        lv_obj_align(state, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_clickable(state, false);
        lv_obj_set_hidden(button, true);
        s_light_buttons[i] = button;
    }
}
}  // namespace

void build_bar_slots(lv_obj_t *bar)
{
    build_media_slot(bar);
    build_heat_slot(bar);
    build_lights_slot(bar);
}

void build_bar_cards(lv_obj_t *screen, lv_obj_t *bar)
{
    build_media_card(screen);
    build_heat_card(screen);
    build_lights_card(screen);
    for (Popout *p : {&s_media_pop, &s_heat_pop, &s_lights_pop}) {
        p->above = bar;
    }
    subscribe(Topic::Media, kNoView, paint_media);
    subscribe(Topic::Home, kNoView, paint_heat);
    subscribe(Topic::Lights, kNoView, paint_lights_slot);
}

lv_obj_t *const *bar_slots(int &count)
{
    static lv_obj_t *slots[3];
    slots[0] = s_media_slot;
    slots[1] = s_heat_slot;
    slots[2] = s_lights_slot;
    count    = 3;
    return slots;
}

}  // namespace ui::detail
