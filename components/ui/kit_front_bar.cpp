// The bar along the top: the status, which opens Setup; the room, the lights
// and the heating; and what plays. Each opens its card, in kit_front_cards.cpp.

#include "kit_front.h"

#include "home_model.h"
#include "kit_fonts.h"
#include "media.h"
#include "media_model.h"
#include "room_model.h"
#include "status_model.h"
#include "topics.h"

#include <cstdio>
#include <cstring>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;

constexpr std::int32_t MEDIA_W = 452, ART = 52;

Status     s_status;
lv_obj_t  *s_lights = nullptr, *s_lights_count = nullptr;
lv_obj_t  *s_heat = nullptr, *s_heat_dot = nullptr, *s_heat_read = nullptr;
lv_obj_t  *s_media = nullptr, *s_title = nullptr, *s_artist = nullptr, *s_play = nullptr;
lv_obj_t  *s_skip = nullptr;  // Skip intro, or Next episode, over the title while it is offered
Cover     *s_cover = nullptr;
std::uint32_t s_covers = 0;

void set_text_if(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

void paint_status()
{
    const detail::StatusState &st = detail::status_state();
    const char *time = detail::pinned_clock() != nullptr ? detail::pinned_clock() : st.time[0] != 0 ? st.time : "--:--";
    set_status(s_status, time, st.wifi, st.present);
}

void paint_lights()
{
    const detail::LightsState &lights = detail::lights_state();
    int on = 0;
    for (bool lit : lights.light_on) {
        on += lit ? 1 : 0;
    }
    char buf[12];
    std::snprintf(buf, sizeof buf, "%d", on);
    set_text_if(s_lights_count, buf);
    lv_obj_t *bulb = lv_obj_get_child(s_lights, 0);
    set_icon(bulb, ic::lightbulb, lights.on);
    lv_obj_set_style_text_color(bulb, lights.on ? lv_color_hex(colours().lamp) : colour(Ink::Secondary), 0);
}

void paint_heat()
{
    const detail::ThermostatState &t = detail::home_state().thermostat;
    char buf[24] = "--";
    if (t.known && t.current_c >= 0.0f) {
        const int tenths = static_cast<int>(t.current_c * 10.0f + 0.5f);
        std::snprintf(buf, sizeof buf, "%d.%d°", tenths / 10, tenths % 10);
    }
    set_text_if(s_heat_read, buf);
    set_dot(s_heat_dot, !t.known || t.state == Hvac::Off ? Ink::Tertiary : t.state == Hvac::Heating ? Ink::Accent : Ink::Amber);
}

void paint_media()
{
    const detail::MediaState &m = detail::media_state();
    set_text_if(s_title, m.has_track ? m.title : m.state[0] != 0 ? m.state : "Nothing playing");
    char artist[128];
    std::snprintf(artist, sizeof artist, "%s", m.has_track ? m.artist : "");
    if (char *nl = std::strchr(artist, '\n')) {
        *nl = 0;  // a video's series: its season and episode are for the card
    }
    set_text_if(s_artist, artist);
    // With nothing playing, play offers the favourites; a player only followed has none.
    lv_obj_set_hidden(s_play, m.has_track && !m.remote);
    set_icon(lv_obj_get_child(s_play, 0), m.has_track && m.playing ? ic::pause : ic::play_arrow, true);
    if (m.covers != s_covers) {
        s_covers = m.covers;
        if (m.art != nullptr && !m.placeholder) {
            s_cover->show(Picture{m.art, m.art_width, media::kArtSize, 0});
        } else {
            s_cover->clear();
        }
    }
}

void check_skip(lv_timer_t *)
{
    const detail::MediaSkip offer = detail::media_skip_offer();
    const bool offered = offer.text != nullptr;
    if (lv_obj_is_hidden(s_skip) == offered) {
        lv_obj_set_hidden(s_skip, !offered);
        lv_obj_set_hidden(s_title, offered);
        lv_obj_set_hidden(s_artist, offered);
    }
    if (offered) {
        set_text_if(lv_obj_get_child(s_skip, 0), offer.text);
    }
}

// A tap on the media: its card. Held: the favourites while nothing plays, or
// the desk to the preset set for what plays, as the cinema's low desk, or
// else what plays fullscreen.
void media_touched(lv_event_t *e)
{
    const detail::MediaState &m = detail::media_state();
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) {
        open_media(lv_event_get_current_target_obj(e));
    } else if (!m.has_track) {
        open_favourites();
    } else if (m.hold_preset >= 0) {
        detail::desk_go_to(m.hold_preset);
    } else {
        open_now_playing();
    }
}

// All the lights on a tap, as the old bar had them; held, each of them.
void lights_touched(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        open_lights(lv_event_get_current_target_obj(e));
    } else if (detail::s_handlers.lights != nullptr) {
        detail::s_handlers.lights();
    }
}

void build_room(lv_obj_t *bar)
{
    lv_obj_t *room = capsule(bar);
    s_lights = capsule_item(room, ic::lightbulb, "0", 118);
    s_lights_count = lv_obj_get_child(s_lights, 1);
    lv_obj_add_event_cb(s_lights, lights_touched, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_lights, lights_touched, LV_EVENT_LONG_PRESSED, nullptr);
    popover_source(s_lights);
    capsule_divider(room);
    s_heat = capsule_item(room, nullptr, "--", 150);
    s_heat_read = lv_obj_get_child(s_heat, 0);
    s_heat_dot  = dot(s_heat, Ink::Tertiary);
    lv_obj_move_to_index(s_heat_dot, 0);
    lv_obj_add_event_cb(s_heat, [](lv_event_t *e) { open_heating(lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);
    popover_source(s_heat);
}

void build_media(lv_obj_t *bar)
{
    s_media = capsule(bar);
    lv_obj_set_width(s_media, MEDIA_W);
    lv_obj_set_style_pad_column(s_media, space::m, 0);
    lv_obj_add_flag(s_media, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_media, colours().glass_solid_opa, LV_STATE_PRESSED);  // the glass firms up under the finger
    s_cover = new Cover(s_media, ART, ART, Shape::Rounded, 12, ic::music_note);
    lv_obj_remove_flag(s_cover->obj(), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *col = flow(s_media, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_flex_grow(col, 1);
    s_title  = text(col, Role::H3, "");
    s_artist = text(col, Role::Small, "", Ink::Secondary);
    const std::int32_t text_w = MEDIA_W - ART - 56 - 4 * space::m;
    for (lv_obj_t *line : {s_title, s_artist}) {
        lv_obj_set_width(line, text_w);
        lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_DOTS);
    }
    // Where the title is, while an intro or the credits run: one tap.
    s_skip = button(col, Button::Accent, nullptr, "Skip intro");
    lv_obj_set_size(s_skip, text_w, 48);
    lv_obj_set_style_radius(s_skip, radius::round, 0);
    lv_obj_add_event_cb(s_skip, [](lv_event_t *) {
        detail::media_skip();
        check_skip(nullptr);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_skip, true);
    lv_timer_create(check_skip, 500, nullptr);

    s_play = button(s_media, Button::Raised, ic::pause, nullptr, true);
    lv_obj_set_size(s_play, 56, 56);
    lv_obj_add_event_cb(s_play, [](lv_event_t *) {
        if (detail::media_state().has_track) {
            detail::media_toggle_play();
        } else {
            open_favourites();
        }
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_media, media_touched, LV_EVENT_SHORT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_media, media_touched, LV_EVENT_LONG_PRESSED, nullptr);
    popover_source(s_media);
}
}  // namespace

void build_bar(lv_obj_t *chrome)
{
    lv_obj_t *bar = flow(chrome, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_pos(bar, inset::left, 16);
    lv_obj_set_size(bar, inset::width, size::bar);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_status = status(bar);
    lv_obj_add_flag(s_status.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_status.root, [](lv_event_t *) { toggle_setup(); }, LV_EVENT_CLICKED, nullptr);
    filler(bar);
    build_claude(bar);
    build_room(bar);
    build_media(bar);

    subscribe(Topic::Status, detail::kNoView, paint_status);
    subscribe(Topic::Lights, detail::kNoView, paint_lights);
    subscribe(Topic::Home, detail::kNoView, paint_heat);
    subscribe(Topic::Media, detail::kNoView, paint_media);
}

}  // namespace ui::kitfront
