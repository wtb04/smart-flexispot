// The bar's cards: what plays, the lights, the heating; and the Claude Code
// sessions' pill and card. A card is made as it opens and goes with its
// popover; what it shows follows the models while it is out.

#include "kit_front.h"

#include "claude.h"
#include "home_model.h"
#include "kit_fonts.h"
#include "media.h"
#include "media_model.h"
#include "room_model.h"
#include "settings_model.h"
#include "topics.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;

void set_text_if(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

void set_hidden_if(lv_obj_t *obj, bool hidden)
{
    if (lv_obj_is_hidden(obj) != hidden) {
        lv_obj_set_hidden(obj, hidden);
    }
}

void set_checked_if(lv_obj_t *obj, bool on)
{
    if (lv_obj_has_state(obj, LV_STATE_CHECKED) != on) {
        lv_obj_set_state(obj, LV_STATE_CHECKED, on);
    }
}

// For labels made here: their ink kept in their user data, so a repaint that changes nothing redraws nothing.
void set_ink_if(lv_obj_t *label, Ink ink)
{
    const auto mark = reinterpret_cast<void *>(static_cast<std::intptr_t>(ink) + 1);
    if (lv_obj_get_user_data(label) != mark) {
        lv_obj_set_user_data(label, mark);
        set_ink(label, ink);
    }
}

// One line, cut short with dots: dots only shorten a label of fixed height.
lv_obj_t *line(lv_obj_t *parent, Role role, Ink ink, std::int32_t width)
{
    lv_obj_t *label = text(parent, role, "", ink);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(label, width, lv_obj_get_style_text_font(label, LV_PART_MAIN)->line_height);
    set_ink_if(label, ink);
    return label;
}

void on_click(lv_obj_t *obj, lv_event_cb_t cb, void *data = nullptr)
{
    lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, data);
}

void *index_data(int i) { return reinterpret_cast<void *>(static_cast<std::intptr_t>(i)); }
int   index_of(lv_event_t *e) { return static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))); }

// A kit part made for a card goes with the card.
void own(Cover *cover)
{
    lv_obj_add_event_cb(cover->obj(), [](lv_event_t *e) { delete static_cast<Cover *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, cover);
}

void own(Rail *rail)
{
    lv_obj_add_event_cb(
        rail->obj(),
        [](lv_event_t *e) {
            auto *r = static_cast<Rail *>(lv_event_get_user_data(e));
            r->set_value(r->value(), false);  // its spring, if it is on one, stopped
            delete r;
        },
        LV_EVENT_DELETE, rail);
}

/** "0:42", "3:07", "1:02:03". */
void write_clock(char *out, std::size_t size, int s)
{
    s = std::max(s, 0);
    if (s >= 3600) {
        std::snprintf(out, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    } else {
        std::snprintf(out, size, "%d:%02d", s / 60, s % 60);
    }
}

// ---- What plays -------------------------------------------------------------

constexpr std::int32_t  MEDIA_W     = 480;
constexpr std::int32_t  MEDIA_INNER = MEDIA_W - 2 * space::xxl;
constexpr std::int32_t  COVER       = 128;
constexpr int           SEEK_S      = 10;
constexpr std::uint32_t GLIDE_MS    = 250;
constexpr std::int32_t  PERMILLE    = 1000;

struct MediaCard : InPsram {
    Cover    *cover = nullptr;
    std::uint32_t covers = UINT32_MAX;
    lv_obj_t *source = nullptr, *title = nullptr, *artist = nullptr, *episode = nullptr;
    lv_obj_t *progress = nullptr, *at = nullptr, *length = nullptr;
    Rail     *bar = nullptr;
    int       at_s = -1, length_s = -1;
    lv_obj_t *transport = nullptr, *prev = nullptr, *play = nullptr, *next = nullptr;
    lv_obj_t *sound = nullptr, *speaker = nullptr, *down = nullptr, *up = nullptr;
    lv_obj_t *volume = nullptr;
    bool      volume_held = false;
    lv_obj_t *favourites = nullptr, *subtitles = nullptr;
    lv_timer_t *glide = nullptr;
    unsigned  shape = UINT_MAX;  // what is shown of it, so it is fitted again only when that changes
};
MediaCard *s_media = nullptr;

void full_view()
{
    if (!detail::media_state().has_track) {
        return;
    }
    close_popover(false);
    open_now_playing();
}

void glide_media(lv_timer_t *)
{
    MediaCard *c = s_media;
    const detail::MediaState &m = detail::media_state();
    if (c == nullptr || !m.has_track || m.duration_s <= 0) {
        return;
    }
    const std::int64_t at_ms = detail::media_position_ms_now();
    c->bar->set_value(static_cast<std::int32_t>(at_ms * PERMILLE / (static_cast<std::int64_t>(m.duration_s) * 1000)), false);
    const int at_s = static_cast<int>(at_ms / 1000);
    if (at_s != c->at_s) {
        c->at_s = at_s;
        char buf[16];
        write_clock(buf, sizeof buf, at_s);
        set_text_if(c->at, buf);
    }
}

lv_timer_t *s_cover_retry = nullptr;

void tint_media_card(const detail::MediaState &m);

void paint_media_card()
{
    MediaCard *c = s_media;
    if (c == nullptr) {
        return;
    }
    const detail::MediaState &m = detail::media_state();
    set_text_if(c->source, m.source);
    set_text_if(c->title, m.has_track ? m.title : "Nothing playing");
    set_ink_if(c->title, m.has_track ? Ink::Primary : Ink::Secondary);
    // One line or two, as the title needs, so who it is by follows it closely.
    lv_point_t size{};
    lv_text_get_size(&size, lv_label_get_text(c->title), fonts::h2(), 0, 0,
                     lv_obj_get_style_width(c->title, LV_PART_MAIN), LV_TEXT_FLAG_NONE);
    const std::int32_t one = fonts::h2()->line_height;
    const bool two_lines = size.y > one;
    if (lv_obj_get_style_height(c->title, LV_PART_MAIN) != (two_lines ? 2 * one : one)) {
        lv_obj_set_height(c->title, two_lines ? 2 * one : one);
    }
    char artist[sizeof m.artist];
    std::snprintf(artist, sizeof artist, "%s", m.has_track ? m.artist : "");
    const char *episode = "";
    if (char *nl = std::strchr(artist, '\n')) {
        *nl     = 0;  // a video's series, and under it its season and episode
        episode = nl + 1;
    }
    set_text_if(c->artist, artist);
    set_text_if(c->episode, episode);
    set_hidden_if(c->episode, episode[0] == 0);

    if (m.covers != c->covers) {
        // at once as the card opens, faded in only as the cover changes while it is open
        const bool opening = c->covers == UINT32_MAX;
        if (!m.has_track || m.art == nullptr || m.placeholder) {
            c->cover->clear(!opening);
            c->covers = m.covers;
        } else if (c->cover->show(Picture{m.art, m.art_width, media::kArtSize, 0}, !opening)) {
            c->covers = m.covers;
        } else {
            again_soon(s_cover_retry, paint_media_card);
        }
        tint_media_card(m);
    }

    const bool timed = m.has_track && m.duration_s > 0;
    const lv_opa_t opa = timed ? LV_OPA_COVER : LV_OPA_TRANSP;  // its room kept, so the card holds still as it comes
    if (lv_obj_get_style_opa(c->progress, LV_PART_MAIN) != opa) {
        lv_obj_set_style_opa(c->progress, opa, 0);
    }
    if (timed && m.duration_s != c->length_s) {
        c->length_s = m.duration_s;
        char buf[16];
        write_clock(buf, sizeof buf, m.duration_s);
        set_text_if(c->length, buf);
    }
    c->at_s = -1;
    glide_media(nullptr);

    // A video's are ten seconds either way, which any player that is steered takes.
    set_hidden_if(c->transport, !m.remote);
    set_hidden_if(c->prev, !m.video && !m.tracks_back);
    set_hidden_if(c->next, !m.video && !m.tracks_on);
    set_icon(lv_obj_get_child(c->prev, 0), m.video ? ic::replay_10 : ic::skip_previous, true);
    set_icon(lv_obj_get_child(c->next, 0), m.video ? ic::forward_10 : ic::skip_next, true);
    set_icon(lv_obj_get_child(c->play, 0), m.has_track && m.playing ? ic::pause : ic::play_arrow, true);
    for (lv_obj_t *b : {c->prev, c->play, c->next}) {
        set_usable(b, m.has_track);
    }

    // Nobody picks another while watching: for a video its subtitles instead of the favourites.
    const bool volume = detail::media_shows_volume();
    const bool subs   = m.video && detail::media_shows_subtitles();
    set_hidden_if(c->sound, !volume && m.video && !subs);
    set_hidden_if(c->speaker, !volume || !m.steps_volume);  // the slider carries its own
    set_hidden_if(c->volume, !volume || m.steps_volume);
    set_hidden_if(c->down, !volume || !m.steps_volume);
    set_hidden_if(c->up, !volume || !m.steps_volume);
    set_usable(c->volume, m.volume >= 0);
    if (m.volume >= 0 && !c->volume_held && lv_slider_get_value(c->volume) != m.volume) {
        lv_slider_set_value(c->volume, m.volume, LV_ANIM_OFF);
    }
    set_hidden_if(c->favourites, m.video);
    set_hidden_if(c->subtitles, !subs);
    set_checked_if(c->subtitles, m.subtitles_shown);
    set_usable(c->subtitles, m.subtitles_available);

    const unsigned shape = (lv_obj_is_hidden(c->transport) ? 1u : 0u) | (lv_obj_is_hidden(c->sound) ? 2u : 0u) |
                           (two_lines ? 4u : 0u) | (episode[0] != 0 ? 8u : 0u);
    if (shape != c->shape) {
        c->shape = shape;
        fit_popover();
    }
}

// The card in its cover's colours: its glass the hue's surface, play in its accent.
void tint_media_card(const detail::MediaState &m)
{
    const bool art = m.has_track && m.art != nullptr && !m.placeholder;
    const AlbumColours c = art ? album_colours(static_cast<const std::uint16_t *>(m.art), m.art_width * media::kArtSize,
                                               look() == Look::Dark)
                               : AlbumColours{};
    tint_popover(c.vivid ? &c.surface : nullptr);
    if (c.vivid) {
        lv_obj_set_style_bg_color(s_media->play, c.accent, 0);
    } else {
        lv_obj_remove_local_style_prop(s_media->play, LV_STYLE_BG_COLOR, 0);
    }
}

void step_clicked(lv_event_t *e)
{
    const int dir = index_of(e);
    if (detail::media_state().video) {
        detail::media_seek_by(dir * SEEK_S);
    } else {
        detail::media_action(dir > 0 ? MediaAction::Next : MediaAction::Previous);
    }
}

// Round fills either side, the one tinted capsule between them, as Apple's
// media card has its controls.
lv_obj_t *transport_button(lv_obj_t *parent, const char *glyph, bool main)
{
    lv_obj_t *b = button(parent, main ? Button::Accent : Button::Raised, nullptr, nullptr, !main);
    lv_obj_set_size(b, size::button_lg, size::button_lg);
    lv_obj_set_style_radius(b, radius::round, 0);
    if (main) {
        lv_obj_set_flex_grow(b, 1);
    }
    lv_obj_center(icon(b, glyph, 34, true, main ? Ink::OnAccent : Ink::Primary));
    return b;
}

// The cover, what plays and who by, how far it has got, and its controls: a
// small Now Playing, the fullscreen one a tap on the cover away.
void build_media_card(lv_obj_t *body)
{
    auto *c = new MediaCard{};
    s_media = c;
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);

    lv_obj_t *head = flow(body, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_width(head, LV_PCT(100));
    c->cover = new Cover(head, COVER, COVER, Shape::Rounded, -1, ic::music_note);
    own(c->cover);
    lv_obj_set_clickable(c->cover->obj(), true);
    on_click(c->cover->obj(), [](lv_event_t *) { full_view(); });
    const std::int32_t text_w = MEDIA_INNER - COVER - space::l;
    lv_obj_t *col = flow(head, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_width(col, text_w);
    lv_obj_set_style_min_height(col, COVER, 0);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    c->source  = line(col, Role::Small, Ink::Secondary, text_w);
    c->title   = line(col, Role::H2, Ink::Primary, text_w);
    c->artist  = line(col, Role::Body, Ink::Secondary, text_w);
    c->episode = line(col, Role::Small, Ink::Tertiary, text_w);

    c->progress = flow(body, LV_FLEX_FLOW_COLUMN, 2);
    lv_obj_set_width(c->progress, LV_PCT(100));
    RailSpec rs;
    rs.length = MEDIA_INNER;
    rs.max    = PERMILLE;
    rs.quiet  = true;
    c->bar = new Rail(c->progress, rs);
    own(c->bar);
    lv_obj_t *times = flow(c->progress, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_width(times, LV_PCT(100));
    lv_obj_set_flex_align(times, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const std::int32_t clock_w = widest(Role::Small, {"88:88:88"});
    c->at     = line(times, Role::Small, Ink::Secondary, clock_w);
    c->length = line(times, Role::Small, Ink::Secondary, clock_w);
    lv_obj_set_style_text_align(c->length, LV_TEXT_ALIGN_RIGHT, 0);

    c->transport = flow(body, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(c->transport, LV_PCT(100));
    lv_obj_set_flex_align(c->transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    c->prev = transport_button(c->transport, ic::skip_previous, false);
    c->play = transport_button(c->transport, ic::play_arrow, true);
    c->next = transport_button(c->transport, ic::skip_next, false);
    on_click(c->prev, step_clicked, index_data(-1));
    on_click(c->next, step_clicked, index_data(1));
    on_click(c->play, [](lv_event_t *) { detail::media_toggle_play(); });

    c->sound = flow(body, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_width(c->sound, LV_PCT(100));
    lv_obj_set_flex_align(c->sound, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    c->speaker = icon(c->sound, ic::volume_up, 28, false, Ink::Secondary);
    const std::int32_t fav_w = widest(Role::H3, {"Favourites"}) + 2 * space::xl;
    c->volume = fill_slider(c->sound, MEDIA_INNER - fav_w - space::m, ic::volume_up);
    lv_obj_add_event_cb(c->volume, [](lv_event_t *) { detail::media_set_volume(lv_slider_get_value(s_media->volume)); },
                        LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(c->volume, [](lv_event_t *) { s_media->volume_held = true; }, LV_EVENT_PRESSED, nullptr);
    for (lv_event_code_t code : {LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
        lv_obj_add_event_cb(c->volume, [](lv_event_t *) { s_media->volume_held = false; }, code, nullptr);
    }
    // A player whose volume only steps, as a display's does: down and up, with no level to show.
    for (int dir : {-1, 1}) {
        lv_obj_t *b = button(c->sound, Button::Raised, dir < 0 ? ic::remove : ic::add, nullptr);
        lv_obj_set_height(b, size::button_lg);
        lv_obj_set_flex_grow(b, 1);
        on_click(b, [](lv_event_t *e) {
            detail::media_action(index_of(e) < 0 ? MediaAction::VolumeDown : MediaAction::VolumeUp);
        }, index_data(dir));
        (dir < 0 ? c->down : c->up) = b;
    }
    c->favourites = button(c->sound, Button::Raised, nullptr, "Favourites");
    lv_obj_set_size(c->favourites, fav_w, size::button_lg);
    on_click(c->favourites, [](lv_event_t *) {
        close_popover(false);
        open_favourites();
    });
    c->subtitles = button(c->sound, Button::Raised, ic::subtitles, nullptr, false, true);
    lv_obj_set_size(c->subtitles, fav_w, size::button_lg);
    on_click(c->subtitles, [](lv_event_t *) { detail::media_toggle_subtitles(); });

    c->glide = lv_timer_create(glide_media, GLIDE_MS, nullptr);
    lv_obj_add_event_cb(body, [](lv_event_t *) {
        lv_timer_delete(s_media->glide);
        delete s_media;
        s_media = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    paint_media_card();
}

// ---- The lights -------------------------------------------------------------

constexpr std::int32_t LIGHTS_W = 430;
constexpr std::int32_t LAMP_W   = (LIGHTS_W - 2 * space::xxl - space::m) / 2;
constexpr std::int32_t LAMP_H   = 120;

struct LightsCard : InPsram {
    lv_obj_t *sub = nullptr, *all = nullptr, *grid = nullptr, *none = nullptr;
    lv_obj_t *tiles[kLightCount]{};
    int       shown = -1;  // how many lamps it has room for
};
LightsCard *s_lights = nullptr;

bool contains(const char *name, const char *word)
{
    char lower[48];
    std::size_t i = 0;
    for (; name[i] != 0 && i + 1 < sizeof lower; ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    lower[i] = 0;
    return std::strstr(lower, word) != nullptr;
}

const char *lamp_glyph(const char *name)
{
    return contains(name, "wall")    ? ic::wall_lamp
           : contains(name, "floor") ? ic::floor_lamp
           : contains(name, "desk")  ? ic::desk
           : contains(name, "main") || contains(name, "ceiling") ? ic::light
                                                                 : ic::lightbulb;
}

void paint_lights_card()
{
    LightsCard *c = s_lights;
    if (c == nullptr) {
        return;
    }
    const detail::LightsState &lights = detail::lights_state();
    int named = 0, on = 0;
    for (int i = 0; i < kLightCount; ++i) {
        const detail::LightState &light = lights.lights[i];
        lv_obj_t *tile = c->tiles[i];
        set_hidden_if(tile, light.name[0] == 0);
        if (light.name[0] == 0) {
            continue;
        }
        ++named;
        on += lights.light_on[i] ? 1 : 0;
        lv_obj_t *name = lv_obj_get_child(lv_obj_get_child(tile, 1), 0);
        if (std::strcmp(lv_label_get_text(name), light.name) != 0) {
            lv_label_set_text(name, light.name);
            set_icon(lv_obj_get_child(tile, 0), lamp_glyph(light.name), lights.light_on[i]);
        }
        if (lv_obj_has_state(tile, LV_STATE_CHECKED) != lights.light_on[i]) {
            set_lamp(tile, lights.light_on[i]);
        }
    }
    char sub[32];
    if (named == 0) {
        sub[0] = 0;
    } else if (on == 0) {
        std::snprintf(sub, sizeof sub, "All off");
    } else {
        std::snprintf(sub, sizeof sub, "%d of %d on", on, named);
    }
    set_text_if(c->sub, sub);
    set_text_if(lv_obj_get_child(c->all, 1), lights.on ? "All off" : "All on");
    set_hidden_if(c->all, named == 0);
    set_hidden_if(c->grid, named == 0);
    set_hidden_if(c->none, named > 0);
    if (named != c->shown) {
        c->shown = named;
        fit_popover();
    }
}

void build_lights_card(lv_obj_t *body)
{
    auto *c  = new LightsCard{};
    s_lights = c;
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, space::l, 0);
    lv_obj_t *head = heading(body, "Lights", "");
    c->sub = lv_obj_get_child(lv_obj_get_child(head, 0), 1);
    c->all = button(head, Button::Raised, ic::power_settings_new, "All off");
    lv_obj_update_layout(c->all);
    lv_obj_set_width(c->all, lv_obj_get_width(c->all) + widest(Role::H3, {"All off", "All on"}) -
                                 widest(Role::H3, {"All off"}));  // the same whichever it offers
    on_click(c->all, [](lv_event_t *) {
        if (detail::s_handlers.lights != nullptr) {
            detail::s_handlers.lights();
        }
    });
    c->grid = flow(body, LV_FLEX_FLOW_ROW_WRAP, space::m);
    lv_obj_set_width(c->grid, LV_PCT(100));
    for (int i = 0; i < kLightCount; ++i) {
        lv_obj_t *t = lamp_tile(c->grid, ic::lightbulb, "", false);
        lv_obj_set_size(t, LAMP_W, LAMP_H);
        on_click(t, [](lv_event_t *e) {
            if (detail::s_handlers.light != nullptr) {
                detail::s_handlers.light(index_of(e));
            }
        }, index_data(i));
        c->tiles[i] = t;
    }
    c->none = empty(body, ic::lightbulb, "No lights from Home Assistant");
    lv_obj_add_event_cb(body, [](lv_event_t *) {
        delete s_lights;
        s_lights = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    paint_lights_card();
}

// ---- The heating ------------------------------------------------------------

constexpr std::int32_t  HEAT_W       = 520;
constexpr std::int32_t  HEAT_INNER   = HEAT_W - 2 * space::xxl;
constexpr std::uint32_t SEND_AFTER_MS = 600;   // the taps stopped: the target goes to the thermostat
constexpr std::uint32_t HOLD_MS      = 5000;  // ours shown until the thermostat says it has it, or this long
constexpr int           NONE         = INT_MIN;

constexpr std::int32_t  HEAT_DIAL    = 360;
constexpr std::int32_t  HEAT_ARC     = 32;
constexpr std::int32_t  HEAT_FOOT    = 56;  // the bottom of the dial's box the arc leaves open
constexpr std::int32_t  HEAT_MODE_GAP = space::xl;  // between the arc's ends and Heat and Off
constexpr std::int32_t  HEAT_H       = 2 * space::xxl + size::button + HEAT_DIAL - space::s - HEAT_FOOT + HEAT_MODE_GAP + 60 + space::m;

struct HeatCard : InPsram {
    lv_obj_t *mode = nullptr;
    Dial     *dial  = nullptr;
    lv_obj_t *toggles[kDialToggleCount]{};
    unsigned  shape = UINT_MAX;
    bool      dragging = false;
};
HeatCard *s_heat = nullptr;

// In tenths of a degree, so steps add up without drifting.
int        s_held     = NONE;  // the target as tapped, until the thermostat has it
std::uint32_t s_held_ms = 0;
lv_timer_t *s_send    = nullptr;

int tenths(float c) { return static_cast<int>(std::lround(c * 10.0f)); }

struct Range {
    int min, max, step;
};
Range range()
{
    const detail::ThermostatState &t = detail::home_state().thermostat;
    if (!t.ranged || t.max_c <= t.min_c) {
        return {150, 300, 5};  // 15 to 30 degrees in halves, until the thermostat tells its own
    }
    return {tenths(t.min_c), tenths(t.max_c), std::max(1, tenths(t.step_c))};
}

int model_target()
{
    const detail::ThermostatState &t = detail::home_state().thermostat;
    return t.known && t.target_c >= 0.0f ? tenths(t.target_c) : NONE;
}

int shown_target()
{
    if (s_held != NONE && s_send == nullptr && (model_target() == s_held || lv_tick_elaps(s_held_ms) > HOLD_MS)) {
        s_held = NONE;
    }
    return s_held != NONE ? s_held : model_target();
}

bool is_off()
{
    const detail::ThermostatState &t = detail::home_state().thermostat;
    if (t.mode[0] != 0 && std::strcmp(t.mode, "--") != 0) {
        char lower[8] = "";
        for (std::size_t i = 0; i + 1 < sizeof lower && t.mode[i] != 0; ++i) {
            lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(t.mode[i])));
        }
        return std::strcmp(lower, "off") == 0;
    }
    return t.state == Hvac::Off;
}

Ink heat_ink(const detail::ThermostatState &t)
{
    return !t.known ? Ink::Tertiary : t.state == Hvac::Heating ? Ink::Accent : t.state == Hvac::Idle ? Ink::Amber : Ink::Tertiary;
}

// Off, the arc is the ink faded, as something paused is: the tertiary ink is only faint as text.
bool heat_off(const detail::ThermostatState &t) { return t.known && t.state != Hvac::Heating && t.state != Hvac::Idle; }

void write_degrees(char *out, std::size_t size, int tenths_c) { std::snprintf(out, size, "%d.%d°", tenths_c / 10, tenths_c % 10); }

void paint_heat_card()
{
    HeatCard *c = s_heat;
    if (c == nullptr) {
        return;
    }
    const detail::HomeState       &home = detail::home_state();
    const detail::ThermostatState &t    = home.thermostat;
    char buf[40] = "";
    const Range r = range();
    c->dial->set_range(r.min, r.max);
    const bool room = t.known && t.current_c >= 0.0f;
    if (room) {
        char now[16];
        write_degrees(now, sizeof now, tenths(t.current_c));
        std::snprintf(buf, sizeof buf, "Room %s", now);
    } else {
        std::snprintf(buf, sizeof buf, "No reading");
    }
    set_text_if(c->dial->caption(), buf);
    c->dial->set_mark(room ? std::clamp(tenths(t.current_c), r.min, r.max) : INT32_MIN);

    const int target = shown_target();
    if (target == NONE) {
        std::snprintf(buf, sizeof buf, "--");
        c->dial->set_value(INT32_MIN);
    } else {
        write_degrees(buf, sizeof buf, target);
        if (!c->dragging) {
            c->dial->set_value(target);
        }
    }
    set_text_if(c->dial->figure(), buf);
    c->dial->set_ink(heat_off(t) ? Ink::Primary : heat_ink(t));
    c->dial->set_faded(heat_off(t));

    set_usable(c->mode, t.known && detail::s_handlers.mode != nullptr);
    segmented_choose(c->mode, is_off() ? 1 : 0, true);

    unsigned shape = 0;
    for (int i = 0; i < kDialToggleCount; ++i) {
        const detail::ToggleState &toggle = home.toggles[i];
        lv_obj_t *row = c->toggles[i];
        set_hidden_if(row, toggle.label[0] == 0);
        shape |= toggle.label[0] != 0 ? 1u << i : 0u;
        set_text_if(lv_obj_get_child(row, 0), toggle.label);
        set_checked_if(row, toggle.on);
    }
    if (shape != c->shape) {
        c->shape = shape;
        fit_popover();
    }
}

void send_target(lv_timer_t *)
{
    lv_timer_delete(s_send);
    s_send    = nullptr;
    s_held_ms = lv_tick_get();
    if (s_held != NONE && detail::s_handlers.setpoint != nullptr) {
        detail::s_handlers.setpoint(static_cast<float>(s_held) / 10.0f);
    }
}

// The target as tapped or dragged to, sent once the finger has been still a moment.
void hold_target(int target)
{
    const Range r = range();
    s_held    = std::clamp(target, r.min, r.max);
    s_held_ms = lv_tick_get();
    if (s_send == nullptr) {
        s_send = lv_timer_create(send_target, SEND_AFTER_MS, nullptr);
    } else {
        lv_timer_reset(s_send);
    }
    paint_heat_card();
}

void dial_moved(lv_event_t *e)
{
    HeatCard *c = s_heat;
    if (c == nullptr || shown_target() == NONE || detail::s_handlers.setpoint == nullptr) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        c->dragging = false;
        c->dial->set_value(shown_target());
        return;
    }
    c->dragging = true;
    const Range r    = range();
    const int   step = (c->dial->value() - r.min + r.step / 2) / r.step * r.step + r.min;
    if (step != s_held || s_send == nullptr) {
        hold_target(step);
    }
}

void build_heat_card(lv_obj_t *body)
{
    auto *c = new HeatCard{};
    s_heat  = c;
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, space::m, 0);
    // What it is doing reads from the arc's ink; the head's right is the toggles'.
    lv_obj_t *head = heading(body, "Heating", nullptr);
    // A toggle's label is what it is set to ("1" or "2" for which radiator, say),
    // not what it does, so it is a small button showing it, lit when on.
    for (int i = 0; i < kDialToggleCount; ++i) {
        lv_obj_t *toggle = button(head, Button::Raised, nullptr, " ", false, true);
        lv_obj_set_style_min_width(toggle, size::button, 0);
        lv_obj_set_style_pad_hor(toggle, space::l, 0);
        lv_obj_add_event_cb(toggle, [](lv_event_t *e) {
            if (detail::s_handlers.dial_toggle != nullptr) {
                detail::s_handlers.dial_toggle(index_of(e));
            }
        }, LV_EVENT_CLICKED, index_data(i));
        c->toggles[i] = toggle;
    }

    DialSpec spec;
    spec.size   = HEAT_DIAL;
    spec.arc    = HEAT_ARC;
    spec.start  = 135;
    spec.sweep  = 270;
    spec.min    = range().min;
    spec.max    = range().max;
    spec.target = true;
    spec.drag   = true;
    spec.mark   = true;
    spec.read   = Role::ReadXL;
    spec.unit   = "";
    c->dial     = new Dial(body, spec);
    lv_obj_t *dial = c->dial->obj();
    lv_obj_set_style_margin_top(dial, -space::s, 0);
    lv_obj_set_width(c->dial->figure(), widest(Role::ReadXL, {"88.8°", "--"}));
    lv_obj_set_style_text_font(c->dial->caption(), fonts::body(), 0);
    lv_obj_set_width(c->dial->caption(), widest(Role::Body, {"Room 88.8°", "No reading"}));
    lv_obj_set_style_margin_top(c->dial->caption(), -space::xs, 0);
    lv_obj_add_event_cb(dial, dial_moved, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(dial, dial_moved, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(dial, [](lv_event_t *e) { delete static_cast<Dial *>(lv_event_get_user_data(e)); },
                        LV_EVENT_DELETE, c->dial);

    c->mode = segmented(body, {"Heat", "Off"}, is_off() ? 1 : 0, HEAT_INNER);
    lv_obj_set_style_margin_top(c->mode, HEAT_MODE_GAP - HEAT_FOOT, 0);  // up into the dial's open foot
    lv_obj_add_event_cb(c->mode, [](lv_event_t *e) {
        // The thermostat's mode goes between the two: a tap on the one it is in already does nothing.
        if ((segmented_chosen(lv_event_get_target_obj(e)) == 1) != is_off() && detail::home_state().thermostat.known &&
            detail::s_handlers.mode != nullptr) {
            detail::s_handlers.mode();
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(body, [](lv_event_t *) {
        delete s_heat;
        s_heat = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    paint_heat_card();
}

// ---- Claude Code ------------------------------------------------------------

constexpr std::int32_t CLAUDE_W     = 720;
constexpr std::int32_t CLAUDE_INNER = CLAUDE_W - 2 * space::xxl;
constexpr int          ROWS         = 4;  // the most urgent; more, one opened, would run off the screen
constexpr std::int32_t PILL_DOT     = 7;
constexpr std::int64_t MS_PER_MIN   = 60 * 1000;
constexpr std::int32_t DETAIL_W     = (CLAUDE_INNER - 56 - 2 * space::l) / 2;

struct SessionRow {
    lv_obj_t *row = nullptr, *glyph = nullptr, *title = nullptr, *doing = nullptr, *state = nullptr, *age = nullptr;
    lv_obj_t *detail = nullptr, *steps = nullptr, *lately = nullptr;
    lv_obj_t *step_dot[claude::kStepCount]{}, *step_text[claude::kStepCount]{};
    lv_obj_t *act[claude::kLatelyCount]{}, *act_when[claude::kLatelyCount]{}, *act_text[claude::kLatelyCount]{};
    claude::State painted = claude::State::Idle;
    bool          glyphed = false;
};

struct ClaudeCard : InPsram {
    lv_obj_t  *sub = nullptr, *more = nullptr;
    SessionRow rows[ROWS];
    unsigned   shape = UINT_MAX;
};

claude::Snapshot *s_snap  = nullptr;  // in PSRAM: internal RAM is short
lv_obj_t         *s_pill  = nullptr, *s_count = nullptr;
lv_obj_t         *s_dots[claude::kSessionCount]{};
ClaudeCard       *s_claude = nullptr;
char              s_opened[sizeof(claude::Session::id)] = "";  // the session whose row is open

struct PillLook {
    int           count = -1;
    bool          amber = false;
    claude::State states[claude::kSessionCount]{};
    bool operator==(const PillLook &) const = default;
};
PillLook s_painted;

Ink ink_of(claude::State state)
{
    switch (state) {
        case claude::State::Working:  return Ink::Accent;
        case claude::State::NeedsYou: return Ink::Amber;
        case claude::State::Done:     return Ink::Green;
        case claude::State::Failed:   return Ink::Red;
        default:                      return Ink::Tertiary;
    }
}

const char *glyph_of(claude::State state)
{
    switch (state) {
        case claude::State::Working:  return ic::sync;
        case claude::State::NeedsYou: return ic::notifications;
        case claude::State::Done:     return ic::check_circle;
        case claude::State::Failed:   return ic::error;
        default:                      return ic::schedule;
    }
}

const char *name_of(claude::State state)
{
    switch (state) {
        case claude::State::NeedsYou: return "Needs you";
        case claude::State::Working:  return "Working";
        case claude::State::Done:     return "Done";
        case claude::State::Failed:   return "Failed";
        default:                      return "Idle";
    }
}

// "4 min", "2 h"; under a minute, "just now".
void write_age(char *out, std::size_t size, std::int64_t ms, bool ago)
{
    const std::int64_t min = ms / MS_PER_MIN;
    if (min < 1) {
        std::snprintf(out, size, "just now");
    } else if (min < 60) {
        std::snprintf(out, size, ago ? "%d min ago" : "%d min", static_cast<int>(min));
    } else {
        std::snprintf(out, size, ago ? "%d h ago" : "%d h", static_cast<int>(min / 60));
    }
}

void paint_pill()
{
    PillLook look;
    look.count = s_snap->count;
    look.amber = s_snap->count > 0 && s_snap->sessions[0].state == claude::State::NeedsYou;  // first, as they are sorted
    for (int i = 0; i < std::min(s_snap->count, claude::kSessionCount); ++i) {
        look.states[i] = s_snap->sessions[i].state;
    }
    if (look == s_painted) {
        return;
    }
    s_painted = look;
    // Amber all over while one waits on the owner; on the glass otherwise, a dot in each one's ink.
    const Palette &p = colours();
    if (look.amber) {
        lv_obj_set_style_bg_color(s_pill, lv_color_hex(p.amber), 0);
        lv_obj_set_style_bg_opa(s_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_count, lv_color_hex(p.on_lamp), 0);
    } else {
        lv_obj_remove_local_style_prop(s_pill, LV_STYLE_BG_COLOR, 0);
        lv_obj_remove_local_style_prop(s_pill, LV_STYLE_BG_OPA, 0);
        lv_obj_remove_local_style_prop(s_count, LV_STYLE_TEXT_COLOR, 0);
    }
    char count[8];
    std::snprintf(count, sizeof count, "%d", s_snap->count);
    set_text_if(s_count, count);
    for (int i = 0; i < claude::kSessionCount; ++i) {
        lv_obj_t *d = s_dots[i];
        set_hidden_if(d, i >= look.count);
        if (i >= look.count) {
            continue;
        }
        const claude::State state = look.states[i];
        if (look.amber) {
            lv_obj_set_style_bg_color(d, lv_color_hex(p.on_lamp), 0);
            lv_obj_set_style_bg_opa(d, state == claude::State::NeedsYou ? LV_OPA_COVER : LV_OPA_40, 0);
        } else {
            lv_obj_set_style_bg_color(d, colour(ink_of(state)), 0);
            lv_obj_set_style_bg_opa(d, state == claude::State::Idle ? p.ink3_opa : static_cast<lv_opa_t>(LV_OPA_COVER), 0);
        }
    }
}

void doing_of(const claude::Session &session, char *out, std::size_t size)
{
    out[0] = 0;
    switch (session.state) {
        case claude::State::NeedsYou:
            claude::describe(session.doing, claude::Tense::Wanted, out, size);
            if (out[0] >= 'a' && out[0] <= 'z') {
                out[0] = static_cast<char>(out[0] - 'a' + 'A');
            }
            if (out[0] == 0) {
                std::snprintf(out, size, "Waiting on you");
            }
            break;
        case claude::State::Working:
            claude::describe(session.doing, claude::Tense::Now, out, size);
            if (out[0] == 0) {
                std::snprintf(out, size, "Thinking");
            }
            if (session.agents > 0) {
                const std::size_t used = std::strlen(out);
                std::snprintf(out + used, size - used, session.agents == 1 ? ", 1 agent" : ", %d agents", session.agents);
            }
            break;
        case claude::State::Done:
            if (session.ran_ms >= MS_PER_MIN) {
                char ran[24];
                write_age(ran, sizeof ran, session.ran_ms, false);
                std::snprintf(out, size, "Finished after %s", ran);
            } else {
                std::snprintf(out, size, "Finished");
            }
            break;
        case claude::State::Failed: std::snprintf(out, size, "Stopped on an error"); break;
        default:                    std::snprintf(out, size, "Waiting for a prompt"); break;
    }
    if (session.steps_total > 0) {
        const std::size_t used = std::strlen(out);
        std::snprintf(out + used, size - used, ", step %d of %d", std::min(session.steps_done + 1, session.steps_total),
                      session.steps_total);
    }
}

void paint_detail(SessionRow &r, const claude::Session &session)
{
    const bool steps = session.step_count > 0;
    set_hidden_if(r.steps, !steps);
    for (int i = 0; i < claude::kStepCount; ++i) {
        const bool shown = i < session.step_count;
        set_hidden_if(lv_obj_get_parent(r.step_text[i]), !shown);
        if (!shown) {
            continue;
        }
        const claude::StepItem &step = session.steps[i];
        set_dot(r.step_dot[i], step.step == claude::Step::Done ? Ink::Green
                               : step.step == claude::Step::Now ? Ink::Accent
                                                                : Ink::Tertiary);
        set_text_if(r.step_text[i], step.text);
        set_ink_if(r.step_text[i], step.step == claude::Step::Now ? Ink::Primary : Ink::Secondary);
    }
    for (int i = 0; i < claude::kLatelyCount; ++i) {
        const bool shown = i < session.lately_count && (!steps || i < claude::kStepCount);
        set_hidden_if(r.act[i], !shown);
        if (!shown) {
            continue;
        }
        const claude::Action &action = session.lately[i];
        char text[64];
        claude::describe(action, claude::Tense::Past, text, sizeof text);
        set_text_if(r.act_text[i], text);
        char when[16];
        write_age(when, sizeof when, s_snap->now_ms - action.at_ms, false);
        set_text_if(r.act_when[i], std::strcmp(when, "just now") == 0 ? "now" : when);
    }
}

void paint_row(SessionRow &r, const claude::Session &session)
{
    const claude::State state = session.state;
    if (!r.glyphed || r.painted != state) {
        r.glyphed = true;
        r.painted = state;
        set_icon(r.glyph, glyph_of(state), true);
        set_ink(r.glyph, ink_of(state));
        set_ink_if(r.state, ink_of(state));
        set_ink_if(r.doing, state == claude::State::NeedsYou ? Ink::Amber : Ink::Secondary);
    }
    char title[64];
    std::snprintf(title, sizeof title, session.machine[0] != 0 ? "%s on %s" : "%s", session.project, session.machine);
    set_text_if(r.title, title);
    char doing[128];
    doing_of(session, doing, sizeof doing);
    set_text_if(r.doing, doing);
    set_text_if(r.state, name_of(state));
    char age[24];
    write_age(age, sizeof age, s_snap->now_ms - session.since_ms, state == claude::State::Done);
    set_text_if(r.age, age);

    const bool opened = std::strcmp(s_opened, session.id) == 0;
    set_hidden_if(r.detail, !opened);
    if (opened) {
        paint_detail(r, session);
    }
}

void paint_claude_card()
{
    ClaudeCard *c = s_claude;
    if (c == nullptr) {
        return;
    }
    const int n = s_snap->count;
    char sub[48];
    if (s_snap->machines > 1) {
        std::snprintf(sub, sizeof sub, "%d sessions on %d laptops", n, s_snap->machines);
    } else {
        std::snprintf(sub, sizeof sub, n == 1 ? "1 session" : "%d sessions", n);
    }
    set_text_if(c->sub, sub);
    unsigned shape = 0;
    for (int i = 0; i < ROWS; ++i) {
        SessionRow &r = c->rows[i];
        set_hidden_if(r.row, i >= n);
        if (i < n) {
            paint_row(r, s_snap->sessions[i]);
        } else {
            set_hidden_if(r.detail, true);
        }
        shape |= (i < n ? 1u : 0u) << (2 * i);
        shape |= (lv_obj_is_hidden(r.detail) ? 0u : 2u) << (2 * i);
        if (!lv_obj_is_hidden(r.detail)) {
            const claude::Session &s = s_snap->sessions[i];
            shape |= static_cast<unsigned>(s.step_count * 8 + std::min(s.lately_count, claude::kLatelyCount)) << 16;
        }
    }
    set_hidden_if(c->more, n <= ROWS);
    if (n > ROWS) {
        int waiting = 0;
        for (int i = ROWS; i < n; ++i) {
            waiting += s_snap->sessions[i].state == claude::State::NeedsYou ? 1 : 0;
        }
        char more[64];
        if (waiting == 0) {
            std::snprintf(more, sizeof more, "%d more, none of them waiting on you", n - ROWS);
        } else {
            std::snprintf(more, sizeof more, "%d more, %d of them waiting on you", n - ROWS, waiting);
        }
        set_text_if(c->more, more);
        shape |= 1u << 30;
    }
    if (shape != c->shape) {
        c->shape = shape;
        fit_popover();
    }
}

void refresh_claude()
{
    claude::snapshot(*s_snap);
    const bool shown = s_snap->shown && detail::settings_state().update.busy == UpdateTarget::None;
    set_hidden_if(s_pill, !shown);
    if (!shown) {
        if (s_claude != nullptr) {
            close_popover(true);
        }
        return;
    }
    paint_pill();
    paint_claude_card();
}

void row_tapped(lv_event_t *e)
{
    const int index = index_of(e);
    if (index >= s_snap->count) {
        return;
    }
    const char *id = s_snap->sessions[index].id;
    if (std::strcmp(s_opened, id) == 0) {
        s_opened[0] = 0;
    } else {
        std::snprintf(s_opened, sizeof s_opened, "%s", id);
    }
    paint_claude_card();
}

lv_obj_t *detail_column(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *col = flow(parent, LV_FLEX_FLOW_COLUMN, space::xs);
    lv_obj_set_width(col, DETAIL_W);
    text(col, Role::Small, caption, Ink::Tertiary);
    return col;
}

void build_session_row(ClaudeCard *c, lv_obj_t *body, int index)
{
    SessionRow &r = c->rows[index];
    RowSpec spec;
    spec.title     = "";
    spec.detail    = "";
    spec.pressable = true;
    r.row = kit::row(body, spec);
    lv_obj_set_style_pad_hor(r.row, 0, 0);
    on_click(r.row, row_tapped, index_data(index));
    lv_obj_t *col = lv_obj_get_child(r.row, 0);
    r.glyph = icon(r.row, ic::sync, 28, true, Ink::Accent);
    lv_obj_move_to_index(r.glyph, 0);
    r.title = lv_obj_get_child(col, 0);
    r.doing = lv_obj_get_child(col, 1);
    const std::int32_t side_w = widest(Role::Body, {"Needs you", "Working", "Failed"});
    lv_obj_t *side = flow(r.row, LV_FLEX_FLOW_COLUMN, 1);
    lv_obj_set_width(side, widest(Role::Small, {"88 min ago"}) > side_w ? widest(Role::Small, {"88 min ago"}) : side_w);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    r.state = text(side, Role::Body, "", Ink::Secondary);
    r.age   = text(side, Role::Small, "", Ink::Tertiary);

    // Opened by a tap: its steps, and what it did last.
    r.detail = flow(body, LV_FLEX_FLOW_ROW, 2 * space::l);
    lv_obj_set_width(r.detail, LV_PCT(100));
    lv_obj_set_style_pad_left(r.detail, 28 + 14, 0);
    lv_obj_set_style_pad_bottom(r.detail, space::s, 0);
    r.steps = detail_column(r.detail, "Steps");
    for (int i = 0; i < claude::kStepCount; ++i) {
        lv_obj_t *l = flow(r.steps, LV_FLEX_FLOW_ROW, space::s);
        lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        r.step_dot[i]  = dot(l, Ink::Tertiary, 8);
        r.step_text[i] = line(l, Role::Small, Ink::Secondary, DETAIL_W - 8 - space::s);
    }
    r.lately = detail_column(r.detail, "Lately");
    const std::int32_t when_w = widest(Role::Small, {"88 min", "now"});
    for (int i = 0; i < claude::kLatelyCount; ++i) {
        r.act[i] = flow(r.lately, LV_FLEX_FLOW_ROW, space::s);
        r.act_when[i] = line(r.act[i], Role::Small, Ink::Tertiary, when_w);
        r.act_text[i] = line(r.act[i], Role::Small, Ink::Primary, DETAIL_W - when_w - space::s);
    }
    lv_obj_set_hidden(r.detail, true);
}

void open_claude(lv_obj_t *source)
{
    lv_obj_t *body = popover(source, CLAUDE_W, 300, From::Below);
    auto *c  = new ClaudeCard{};
    s_claude = c;
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 0, 0);
    lv_obj_t *head = heading(body, "Claude Code", "");
    lv_obj_set_style_pad_bottom(head, space::s, 0);
    c->sub = lv_obj_get_child(lv_obj_get_child(head, 0), 1);
    for (int i = 0; i < ROWS; ++i) {
        build_session_row(c, body, i);
    }
    c->more = text(body, Role::Small, "", Ink::Secondary);
    lv_obj_set_style_pad_top(c->more, space::s, 0);
    lv_obj_add_event_cb(body, [](lv_event_t *) {
        delete s_claude;
        s_claude = nullptr;
    }, LV_EVENT_DELETE, nullptr);
    paint_claude_card();
}

void build_pill(lv_obj_t *bar)
{
    s_snap = new (lv_malloc(sizeof(claude::Snapshot))) claude::Snapshot{};
    s_pill = capsule(bar);
    lv_obj_set_width(s_pill, size::bar);
    lv_obj_set_style_pad_hor(s_pill, 0, 0);
    lv_obj_set_flex_flow(s_pill, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_pill, 3, 0);
    lv_obj_set_clickable(s_pill, true);
    lv_obj_set_style_bg_opa(s_pill, colours().glass_solid_opa, LV_STATE_PRESSED);
    s_count = text(s_pill, Role::H3, "0");
    lv_obj_t *dots = flow(s_pill, LV_FLEX_FLOW_ROW, 3);
    for (lv_obj_t *&d : s_dots) {
        d = lv_obj_create(dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, PILL_DOT, PILL_DOT);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_clickable(d, false);
        lv_obj_set_hidden(d, true);
    }
    on_click(s_pill, [](lv_event_t *e) { open_claude(lv_event_get_current_target_obj(e)); });
    popover_source(s_pill);
    on_restyle(s_pill, [](void *) {
        s_painted = PillLook{};  // its amber and its dots' inks are the look's
        if (!lv_obj_is_hidden(s_pill)) {
            paint_pill();
        }
    }, nullptr);
    lv_obj_set_hidden(s_pill, true);

    subscribe(Topic::Claude, detail::kNoView, refresh_claude);
    subscribe(Topic::Update, detail::kNoView, refresh_claude);
    // The ages go on, and ten quiet minutes take the pill away.
    subscribe(Topic::Second, detail::kNoView, [] {
        if (!lv_obj_is_hidden(s_pill)) {
            refresh_claude();
        }
    });
}
}  // namespace

void open_media(lv_obj_t *source) { build_media_card(popover(source, MEDIA_W, 400, From::Below)); }
void open_lights(lv_obj_t *source) { build_lights_card(popover(source, LIGHTS_W, 300, From::Below)); }
void open_heating(lv_obj_t *source) { build_heat_card(popover(source, HEAT_W, HEAT_H, From::Below)); }

void build_claude(lv_obj_t *bar)
{
    build_pill(bar);
    // What the cards show follows the models while one is out.
    subscribe(Topic::Media, detail::kNoView, paint_media_card);
    subscribe(Topic::Lights, detail::kNoView, paint_lights_card);
    subscribe(Topic::Home, detail::kNoView, paint_heat_card);
}
}  // namespace ui::kitfront
