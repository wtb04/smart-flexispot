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
constexpr std::int32_t MEDIA_W      = 350;
constexpr std::int32_t HEAT_W       = 210;
constexpr std::int32_t LIGHTS_W     = 170;
constexpr std::int32_t THUMB        = 52;
constexpr std::int32_t THUMB_RADIUS = 10;
constexpr std::int32_t PLAY_W       = 68;
constexpr std::int32_t TEXT_GAP     = 12;
constexpr std::int32_t HEAT_DOT     = 12;

constexpr std::uint32_t CARD_IDLE_MS = 20 * 1000;
constexpr std::int32_t  CARD_PAD     = 24;
constexpr std::int32_t  CARD_GAP     = 12;

constexpr std::int32_t  MEDIA_CARD_W = 460;
constexpr std::int32_t  COVER        = 112;
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
    // Nothing behind it at rest, so the bar reads as what it says rather than
    // as a row of buttons; a shape only while pressed, or while its card is out.
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
    for (const lv_state_t state : {LV_STATE_PRESSED, LV_STATE_CHECKED}) {
        lv_obj_set_style_bg_color(slot, lv_color_hex(theme::panel), state);
        lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, state);
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
    lv_obj_t      *frame = nullptr;
    lv_obj_t      *image = nullptr;
    lv_obj_t      *mark  = nullptr;  // shown while there is none
    lv_image_dsc_t dsc[2]{};         // in turn, so the one on screen is never rewritten under it
    int            slot   = 0;
    std::uint32_t  covers = UINT32_MAX;
};

void build_cover(Cover &cover, lv_obj_t *parent, std::int32_t side, std::int32_t radius)
{
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
lv_obj_t *s_slot_play   = nullptr;

Popout    s_media_pop;
Cover     s_card_cover;
lv_obj_t *s_card_source = nullptr;
lv_obj_t *s_card_title  = nullptr;
lv_obj_t *s_card_artist = nullptr;
lv_obj_t *s_card_prev   = nullptr;
lv_obj_t *s_card_next   = nullptr;
lv_obj_t *s_volume      = nullptr;
lv_obj_t *s_volume_fill = nullptr;
lv_obj_t *s_volume_text = nullptr;
bool      s_volume_held = false;

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

void paint_media()
{
    const MediaState &media = media_state();
    char              artist[sizeof(media.artist)];
    first_line(media.artist, artist, sizeof(artist));
    theme::set_text(s_slot_title, media.has_track ? media.title : "Nothing playing");
    theme::set_text_color(s_slot_title, media.has_track ? theme::text : theme::secondary);
    show_play(s_slot_play, media.has_track && media.playing);
    // With nothing playing, play offers the favourites.
    theme::set_usable(s_slot_play, !media.has_track || media.remote);
    paint_cover(s_slot_cover);

    if (s_card_title == nullptr) {
        return;
    }
    theme::set_text(s_card_source, media.source);
    theme::set_text(s_card_title, media.has_track ? media.title : "Nothing playing");
    theme::set_text(s_card_artist, media.has_track ? artist : "");
    theme::set_text(lv_obj_get_child(s_card_prev, 0), media.video ? "-10 s" : LV_SYMBOL_PREV);
    theme::set_text(lv_obj_get_child(s_card_next, 0), media.video ? "+10 s" : LV_SYMBOL_NEXT);
    for (lv_obj_t *button : {s_card_prev, s_card_next}) {
        theme::set_usable(button, media.has_track && media.remote);
    }
    theme::set_usable(s_volume, media.volume >= 0);
    if (media.volume >= 0 && !s_volume_held) {
        show_volume(media.volume);
    }
    paint_cover(s_card_cover);
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

void build_media_slot(lv_obj_t *bar)
{
    s_media_slot = make_slot(bar, MEDIA_W);
    lv_obj_add_event_cb(s_media_slot, [](lv_event_t *) { open_under(s_media_pop, s_media_slot); }, LV_EVENT_CLICKED,
                        nullptr);
    build_cover(s_slot_cover, s_media_slot, THUMB, THUMB_RADIUS);
    lv_obj_align(s_slot_cover.frame, LV_ALIGN_LEFT_MID, 0, 0);

    const std::int32_t text_x = THUMB + TEXT_GAP;
    const std::int32_t text_w = MEDIA_W - 2 * SLOT_PAD - text_x - PLAY_W - TEXT_GAP;
    s_slot_title              = one_line(s_media_slot, theme::text, fonts::size_20(), text_w);
    lv_obj_align(s_slot_title, LV_ALIGN_LEFT_MID, text_x, 0);

    s_slot_play = theme::make_button(s_media_slot, LV_SYMBOL_PLAY, theme::panel_light, fonts::size_28());
    lv_obj_set_size(s_slot_play, PLAY_W, BTN_H);
    lv_obj_set_style_radius(s_slot_play, BTN_H / 2, 0);
    theme::fill_accent(s_slot_play, LV_STATE_CHECKED);
    lv_obj_align(s_slot_play, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_slot_play, play_clicked, LV_EVENT_CLICKED, nullptr);
}

lv_obj_t *card_button(lv_obj_t *parent, const char *text, std::int32_t w)
{
    lv_obj_t *button = theme::make_button(parent, text, theme::panel_light, fonts::size_22());
    lv_obj_set_size(button, w, CARD_BTN_H);
    return button;
}

void build_media_card(lv_obj_t *screen)
{
    lv_obj_t *card    = build_popout(s_media_pop, screen, MEDIA_CARD_W, GAP, GAP, CARD_IDLE_MS);
    s_media_pop.button = s_media_slot;
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(card, CARD_GAP, 0);
    lv_obj_set_style_pad_column(card, CARD_GAP, 0);
    const std::int32_t inner = MEDIA_CARD_W - 2 * CARD_PAD;

    // The cover opens the fullscreen view, as the card on Home did.
    build_cover(s_card_cover, card, COVER, theme::radius::control);
    lv_obj_set_clickable(s_card_cover.frame, true);
    lv_obj_add_event_cb(s_card_cover.frame, [](lv_event_t *) {
        if (!media_state().has_track) {
            return;
        }
        open_popout(s_media_pop, false);
        media_is_video() ? open_cinema() : open_music();
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *text = lv_obj_create(card);
    lv_obj_remove_style_all(text);
    lv_obj_set_size(text, inner - COVER - CARD_GAP, COVER);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(text, theme::space::xs, 0);
    lv_obj_set_clickable(text, false);
    const std::int32_t text_w = inner - COVER - CARD_GAP;
    s_card_source = one_line(text, theme::secondary, theme::type_label(), text_w);
    s_card_title  = one_line(text, theme::text, fonts::size_22(), text_w);
    s_card_artist = one_line(text, theme::secondary, fonts::size_16(), text_w);

    const std::int32_t half = (inner - CARD_GAP) / 2;
    s_card_prev = card_button(card, LV_SYMBOL_PREV, half);
    lv_obj_add_event_cb(s_card_prev, step_clicked, LV_EVENT_CLICKED, nullptr);
    s_card_next = card_button(card, LV_SYMBOL_NEXT, half);
    lv_obj_add_event_cb(s_card_next, step_clicked, LV_EVENT_CLICKED, s_card_next);

    // The volume, as wide as the music view's is tall, the favourites beside it.
    const std::int32_t fav_w = CARD_BTN_H + 2 * CARD_GAP;
    s_volume = lv_obj_create(card);
    theme::style_panel(s_volume, theme::panel_light, theme::radius::control);
    lv_obj_set_size(s_volume, inner - fav_w - CARD_GAP, CARD_BTN_H);
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

    lv_obj_t *favourites = card_button(card, LV_SYMBOL_LIST, fav_w);
    lv_obj_add_event_cb(favourites, [](lv_event_t *) {
        open_popout(s_media_pop, false);
        open_favourites();
    }, LV_EVENT_CLICKED, nullptr);
    fit_popout(s_media_pop);
}

// ---- The heating ----

lv_obj_t *s_heat_slot   = nullptr;
lv_obj_t *s_heat_dot    = nullptr;
lv_obj_t *s_heat_now    = nullptr;
lv_obj_t *s_heat_target = nullptr;
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
    degrees(s_heat_now, "", t.known ? t.current_c : -1.0f);
    const bool off = !t.known || t.state == Hvac::Off;
    if (off) {
        theme::set_text(s_heat_target, t.known ? "off" : "");
    } else {
        degrees(s_heat_target, LV_SYMBOL_RIGHT " ", t.target_c);
    }
    const std::uint32_t ink = t.state == Hvac::Heating ? theme::primary : t.state == Hvac::Idle ? theme::amber : theme::secondary;
    theme::set_bg_color(s_heat_dot, off ? theme::secondary : ink);
    theme::set_text_color(s_heat_target, theme::secondary);
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
    s_heat_now    = theme::make_label(s_heat_slot, "--", theme::text, fonts::size_28());
    s_heat_target = theme::make_label(s_heat_slot, "", theme::secondary, fonts::size_20());
    for (lv_obj_t *label : {s_heat_now, s_heat_target}) {
        lv_obj_set_clickable(label, false);
    }
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
    char text[16] = "Off";
    if (lights.on) {
        if (on > 0) {
            std::snprintf(text, sizeof(text), "%d on", on);
        } else {
            std::snprintf(text, sizeof(text), "On");
        }
    }
    // On in the accent, rather than the whole slot filled with it.
    theme::set_text(s_lights_text, text);
    theme::set_text_color(s_lights_text, lights.on ? theme::primary : theme::secondary);
    lv_obj_set_style_image_recolor(s_lights_bulb, lv_color_hex(lights.on ? theme::primary : theme::secondary), 0);

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
    s_lights_bulb = lv_image_create(s_lights_slot);
    lv_image_set_src(s_lights_bulb, &icons::bulb_glass_icon);
    lv_obj_set_style_image_recolor_opa(s_lights_bulb, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_lights_bulb, false);
    s_lights_text = theme::make_label(s_lights_slot, "Off", theme::secondary, fonts::size_22());
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
