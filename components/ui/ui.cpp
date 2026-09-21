#include "ui.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "modal_overlay.h"
#include "segment_display.h"
#include "theme.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <optional>
#include <utility>

namespace ui {
namespace {

constexpr char TAG[] = "ui";

// Generous, because every setter here is called from a task that is not the
// UI: 100 ms was short enough that a push landing during the first full-screen
// render simply gave up, and that slot kept whatever it had.
constexpr std::uint32_t LOCK_TIMEOUT_MS = 500;

// Everything is sized from the real display rather than with percentages:
// LV_PCT() returns an encoded sentinel, so LV_PCT(100) - something is not a
// width, it is nonsense that silently lays out wrong.
constexpr std::int32_t RAIL_W      = 330;
constexpr std::int32_t NAV_H       = 92;
constexpr std::int32_t RAIL_BTN_H  = 124;
constexpr std::int32_t GAP        = 16;
constexpr std::int32_t EDGE_GAP   = 0;   // navigation sits on the bottom edge
// Matches the gap above the page, so the content sits centred between the top
// edge and the navigation bar rather than crowding the buttons.
constexpr std::int32_t NAV_GAP    = GAP;

// One inset and one gap used by every group of controls, so the rail, the
// drawer and the navigation bar space their buttons identically instead of
// each carrying its own hand-picked number.
constexpr std::int32_t PANEL_PAD  = 16;
constexpr std::int32_t BUTTON_GAP = 16;

struct Layout {
    std::int32_t screen_w;
    std::int32_t screen_h;
    std::int32_t content_x;  // left edge of everything right of the rail
    std::int32_t content_w;
    std::int32_t content_h;  // above the navigation bar
};

Layout layout()
{
    lv_display_t      *disp = lv_display_get_default();
    const std::int32_t w    = lv_display_get_horizontal_resolution(disp);
    const std::int32_t h    = lv_display_get_vertical_resolution(disp);
    const std::int32_t x      = RAIL_W + GAP;
    const std::int32_t area_h = h - GAP - EDGE_GAP;
    return Layout{w, h, x, w - x - GAP, area_h - NAV_H - NAV_GAP};
}

Handlers s_handlers{};

// Everything that commands the desk, so it can be dimmed as a group when the
// control box is not answering.
constexpr int DESK_CONTROL_MAX = 8;
lv_obj_t     *s_desk_controls[DESK_CONTROL_MAX] = {};
int           s_desk_control_count              = 0;
bool          s_desk_available                  = true;

void register_desk_control(lv_obj_t *obj)
{
    if (s_desk_control_count < DESK_CONTROL_MAX) {
        s_desk_controls[s_desk_control_count++] = obj;
    }
}
int               s_initial_brightness = 80;

std::optional<SegmentDisplay> s_height;
lv_obj_t *s_wifi_icon     = nullptr;
lv_obj_t *s_wifi_slash    = nullptr;
lv_obj_t *s_phone_icon    = nullptr;
lv_obj_t *s_phone_slash   = nullptr;
lv_obj_t *s_clock_label   = nullptr;

// --- small helpers ---------------------------------------------------------

// --- notifications ---------------------------------------------------------

constexpr int          NOTIFY_QUEUE_LEN  = 4;
constexpr int          NOTIFY_DEFAULT_MS = 8000;
constexpr std::int32_t NOTIFY_W          = 640;
constexpr std::int32_t NOTIFY_H          = 220;

struct Notice {
    char title[64];
    char message[192];
    char level[12];
    int  timeout_ms;
};

Notice      s_notice_queue[NOTIFY_QUEUE_LEN];
int         s_notice_count = 0;
lv_obj_t   *s_notice_scrim = nullptr;
lv_obj_t   *s_notice_card  = nullptr;
lv_obj_t   *s_notice_bar   = nullptr;
lv_obj_t   *s_notice_title = nullptr;
lv_obj_t   *s_notice_body  = nullptr;
lv_timer_t *s_notice_timer = nullptr;

std::uint32_t level_colour(const char *level)
{
    if (std::strcmp(level, "error") == 0) return theme::red;
    if (std::strcmp(level, "warning") == 0) return theme::amber;
    if (std::strcmp(level, "success") == 0) return theme::green;
    return theme::orange;
}

void hide_notice()
{
    lv_obj_set_hidden(s_notice_card, true);
    lv_obj_set_hidden(s_notice_scrim, true);
    if (s_notice_timer != nullptr) {
        lv_timer_pause(s_notice_timer);
    }
}

void show_next_notice()
{
    if (s_notice_count == 0) {
        hide_notice();
        return;
    }
    const Notice notice = s_notice_queue[0];
    for (int i = 1; i < s_notice_count; ++i) {
        s_notice_queue[i - 1] = s_notice_queue[i];
    }
    --s_notice_count;

    lv_obj_set_style_bg_color(s_notice_bar, lv_color_hex(level_colour(notice.level)), 0);
    // A message with no title reads better promoted into the heading than set
    // as small print under an empty one.
    if (notice.title[0] != '\0') {
        lv_label_set_text(s_notice_title, notice.title);
        lv_label_set_text(s_notice_body, notice.message);
    } else {
        lv_label_set_text(s_notice_title, notice.message);
        lv_label_set_text(s_notice_body, "");
    }

    lv_obj_set_hidden(s_notice_scrim, false);
    lv_obj_move_foreground(s_notice_scrim);
    lv_obj_set_hidden(s_notice_card, false);
    lv_obj_move_foreground(s_notice_card);

    const int timeout = notice.timeout_ms > 0 ? notice.timeout_ms : NOTIFY_DEFAULT_MS;
    if (s_notice_timer != nullptr) {
        lv_timer_set_period(s_notice_timer, static_cast<std::uint32_t>(timeout));
        lv_timer_reset(s_notice_timer);
        lv_timer_resume(s_notice_timer);
    }
}

void notice_timeout_cb(lv_timer_t *) { show_next_notice(); }
void notice_tapped_cb(lv_event_t *) { show_next_notice(); }

// On the top layer so no page layout can cover or displace it.
void create_notice_card()
{
    const Layout l = layout();

    // Everything right of the rail: the page and the navigation under it. A
    // notification has nothing to do with the page it happens to land on, so it
    // should not be possible to navigate out from under one -- the scrim
    // swallows every tap and dismisses, the way the card itself does. The rail
    // stays lit and live, so the desk can still be driven while a message is up.
    s_notice_scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_notice_scrim, RAIL_W, 0);
    lv_obj_set_size(s_notice_scrim, l.screen_w - RAIL_W, l.screen_h);
    theme::style_panel(s_notice_scrim, theme::background, 0);
    lv_obj_set_style_bg_opa(s_notice_scrim, LV_OPA_70, 0);
    lv_obj_set_hidden(s_notice_scrim, true);
    lv_obj_set_clickable(s_notice_scrim, true);
    lv_obj_add_event_cb(s_notice_scrim, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);

    s_notice_card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_notice_card, NOTIFY_W, NOTIFY_H);
    // On the top layer so no page can cover it, but centred over the content
    // area rather than the whole screen: centring it over everything puts it
    // half behind the rail.
    lv_obj_align(s_notice_card, LV_ALIGN_CENTER,
                 l.content_x + l.content_w / 2 - l.screen_w / 2,
                 GAP + l.content_h / 2 - l.screen_h / 2);
    theme::style_panel(s_notice_card, theme::panel_light);
    lv_obj_set_style_border_width(s_notice_card, 2, 0);
    lv_obj_set_style_border_color(s_notice_card, lv_color_hex(theme::panel_light), 0);
    lv_obj_set_hidden(s_notice_card, true);
    lv_obj_add_event_cb(s_notice_card, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);

    s_notice_bar = lv_obj_create(s_notice_card);
    lv_obj_set_size(s_notice_bar, 10, NOTIFY_H - 40);
    lv_obj_align(s_notice_bar, LV_ALIGN_LEFT_MID, -8, 0);
    lv_obj_set_style_border_width(s_notice_bar, 0, 0);
    lv_obj_set_style_radius(s_notice_bar, 5, 0);

    s_notice_title = lv_label_create(s_notice_card);
    lv_obj_set_width(s_notice_title, NOTIFY_W - 80);
    lv_label_set_long_mode(s_notice_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_notice_title, LV_ALIGN_TOP_LEFT, 24, 8);
    lv_obj_set_style_text_font(s_notice_title, fonts::size_28(), 0);
    // Explicit: the card is dark and the inherited theme colour is not.
    lv_obj_set_style_text_color(s_notice_title, lv_color_hex(theme::text), 0);

    s_notice_body = lv_label_create(s_notice_card);
    lv_obj_set_width(s_notice_body, NOTIFY_W - 80);
    lv_label_set_long_mode(s_notice_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_notice_body, LV_ALIGN_TOP_LEFT, 24, 76);
    lv_obj_set_style_text_color(s_notice_body, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_text_font(s_notice_body, fonts::size_28(), 0);

    s_notice_timer = lv_timer_create(notice_timeout_cb, NOTIFY_DEFAULT_MS, nullptr);
    lv_timer_pause(s_notice_timer);
}

// --- controls --------------------------------------------------------------

void move_event_cb(lv_event_t *e)
{
    if (s_handlers.move == nullptr) {
        return;
    }
    const auto direction =
        static_cast<Move>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_handlers.move(lv_event_get_code(e) == LV_EVENT_PRESSED ? direction : Move::Stop);
}

// Press starts the motion, release stops it. PRESS_LOST matters as much as
// RELEASED: a finger sliding off must not leave the desk travelling.
void create_move_button(lv_obj_t *parent, const char *symbol, Move direction, std::int32_t w,
                        std::int32_t h)
{
    // Through the shared helper rather than styled by hand: doing it manually
    // left LVGL's default shadow in place, which showed as a ring under the
    // button.
    lv_obj_t *btn = theme::make_button(parent, symbol, theme::panel_light,
                                       fonts::size_48());
    lv_obj_set_size(btn, w, h);

    auto *user_data = reinterpret_cast<void *>(
        static_cast<std::intptr_t>(std::to_underlying(direction)));
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_RELEASED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESS_LOST, user_data);
    register_desk_control(btn);
}

lv_obj_t *icon_bar(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                   std::int32_t h)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    theme::style_panel(bar, theme::orange, 2);
    lv_obj_set_clickable(bar, false);
    return bar;
}

// A desk drawn from five rectangles: top, two legs, two feet. The legs get
// shorter for the sit variant, which is the whole point of the icon.
void add_desk_icon(lv_obj_t *button, bool high)
{
    lv_obj_t *icon = lv_obj_create(button);
    lv_obj_set_size(icon, 58, 56);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);
    theme::style_panel(icon, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(icon, false);

    const std::int32_t top = high ? 7 : 20;
    icon_bar(icon, 2, top, 54, 7);
    icon_bar(icon, 8, top + 7, 6, 45 - top);
    icon_bar(icon, 44, top + 7, 6, 45 - top);
    icon_bar(icon, 4, 49, 14, 5);
    icon_bar(icon, 40, 49, 14, 5);
}

lv_obj_t *make_rail_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = theme::make_button(parent, text);
    lv_obj_set_size(btn, RAIL_W - 2 * PANEL_PAD, RAIL_BTN_H);
    register_desk_control(btn);
    return btn;
}

void preset_clicked_cb(lv_event_t *e)
{
    if (s_handlers.preset != nullptr) {
        s_handlers.preset(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))),
                    lv_event_get_code(e) == LV_EVENT_LONG_PRESSED);
    }
}

void manual_clicked_cb(lv_event_t *);
lv_obj_t *s_drawer        = nullptr;
lv_obj_t *s_drawer_toggle = nullptr;
bool      s_drawer_open   = false;

void create_rail(lv_obj_t *parent)
{
    const Layout l = layout();

    lv_obj_t *rail = lv_obj_create(parent);
    lv_obj_set_pos(rail, 0, 0);
    lv_obj_set_size(rail, RAIL_W, l.screen_h);
    theme::style_panel(rail);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_set_style_pad_all(rail, PANEL_PAD, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rail, BUTTON_GAP, 0);

    // Clock and the three link glyphs share one strip, so the rail carries all
    // the ambient state and the rest of the screen is free for pages.
    lv_obj_t *strip = lv_obj_create(rail);
    lv_obj_set_size(strip, RAIL_W - 2 * PANEL_PAD, 44);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_scrollable(strip, false);

    s_clock_label = theme::make_label(strip, "--:--", theme::text, fonts::size_28());
    lv_obj_align(s_clock_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_wifi_icon = theme::make_label(strip, LV_SYMBOL_WIFI, theme::text, fonts::size_22());
    lv_obj_align(s_wifi_icon, LV_ALIGN_RIGHT_MID, 0, 0);

    // A slash over the glyph, rather than a second icon: LVGL has no
    // wifi-off symbol and this reads instantly.
    s_wifi_slash = theme::make_label(strip, "/", theme::text, fonts::size_32());
    lv_obj_align_to(s_wifi_slash, s_wifi_icon, LV_ALIGN_CENTER, 0, -2);

    // Presence, in the same language as the link glyphs. Drawn rather than set
    // from the font: LVGL's phone symbol is a corded handset, which is not what
    // is being tracked. The colour never changes -- the slash is the state.
    s_phone_icon = lv_obj_create(strip);
    lv_obj_set_size(s_phone_icon, 22, 32);
    theme::style_panel(s_phone_icon, theme::text, 5);
    lv_obj_set_clickable(s_phone_icon, false);
    lv_obj_align(s_phone_icon, LV_ALIGN_RIGHT_MID, -46, 0);

    lv_obj_t *phone_screen = lv_obj_create(s_phone_icon);
    lv_obj_set_size(phone_screen, 16, 22);
    lv_obj_align(phone_screen, LV_ALIGN_TOP_MID, 0, 4);
    theme::style_panel(phone_screen, theme::panel, 2);
    lv_obj_set_clickable(phone_screen, false);

    s_phone_slash = theme::make_label(strip, "/", theme::text, fonts::size_32());
    lv_obj_align_to(s_phone_slash, s_phone_icon, LV_ALIGN_CENTER, 0, -2);

    theme::make_label(rail, "DESK HEIGHT", theme::secondary, &lv_font_montserrat_18);

    lv_obj_t *readout = lv_obj_create(rail);
    lv_obj_set_size(readout, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    theme::style_panel(readout, theme::panel, 0);
    lv_obj_set_style_bg_opa(readout, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(readout, LV_FLEX_FLOW_COLUMN);
    // END on the cross axis right-aligns the unit under the digits, so it sits
    // at the bottom-right corner of the readout rather than beside it.
    lv_obj_set_flex_align(readout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_bottom(readout, 18, 0);

    s_height.emplace(readout);
    s_height->set_tenths(-1, theme::orange);

    // Outside the flex flow and pinned to the right edge, level with the
    // bottom of the digits -- a unit label reads as part of the number, not as
    // another row under it. The dashes already say "no reading", so there is
    // no status line here either.
    theme::make_label(readout, "CM", theme::secondary, fonts::size_22());

    // Presets 3 and 4 are the two anyone actually uses, so they get names and
    // the space. The rest live behind Manual.
    lv_obj_t *stand = make_rail_button(rail, "STAND");
    add_desk_icon(stand, true);
    lv_obj_align(lv_obj_get_child(stand, 0), LV_ALIGN_CENTER, 34, 0);
    lv_obj_add_event_cb(stand, preset_clicked_cb, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(2)));
    lv_obj_add_event_cb(stand, preset_clicked_cb, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(2)));

    lv_obj_t *sit = make_rail_button(rail, "SIT");
    add_desk_icon(sit, false);
    lv_obj_align(lv_obj_get_child(sit, 0), LV_ALIGN_CENTER, 34, 0);
    lv_obj_add_event_cb(sit, preset_clicked_cb, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(3)));
    lv_obj_add_event_cb(sit, preset_clicked_cb, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(3)));

    // Small and out of the way: the extra controls are occasional, and a
    // full-width button for them competed with STAND and SIT.
    s_drawer_toggle = theme::make_button(rail, LV_SYMBOL_RIGHT);
    lv_obj_set_size(s_drawer_toggle, 84, 64);
    lv_obj_set_ignore_layout(s_drawer_toggle, true);
    lv_obj_align(s_drawer_toggle, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(s_drawer_toggle, manual_clicked_cb, LV_EVENT_CLICKED, nullptr);
}

// The sidebar widens instead of a card appearing over the middle of the
// screen: the extra controls belong to the rail, so they should look like part
// of it sliding out, not like a separate thing floating on top.
constexpr std::int32_t DRAWER_W  = 340;
constexpr std::uint32_t DRAWER_MS = 200;

void drawer_width_cb(void *target, std::int32_t value)
{
    lv_obj_set_width(static_cast<lv_obj_t *>(target), value);
}

void animate_drawer(bool open)
{
    s_drawer_open = open;
    // The arrow points the way the drawer will go, so the control says what it
    // is about to do rather than what state it is in.
    if (s_drawer_toggle != nullptr) {
        theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                        open ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_drawer);
    lv_anim_set_exec_cb(&anim, drawer_width_cb);
    lv_anim_set_values(&anim, lv_obj_get_width(s_drawer), open ? DRAWER_W : 0);
    lv_anim_set_duration(&anim, DRAWER_MS);
    lv_anim_set_path_cb(&anim, open ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&anim);
}

void manual_clicked_cb(lv_event_t *) { animate_drawer(!s_drawer_open); }

void create_drawer(lv_obj_t *parent)
{
    const Layout l = layout();

    s_drawer = lv_obj_create(parent);
    lv_obj_set_pos(s_drawer, RAIL_W, 0);
    lv_obj_set_size(s_drawer, 0, l.screen_h);
    theme::style_panel(s_drawer, theme::panel, 0);
    // Children are clipped to the object, so at zero width the buttons are
    // simply not drawn and the drawer reads as closed.
    lv_obj_set_style_pad_all(s_drawer, PANEL_PAD, 0);
    lv_obj_set_flex_flow(s_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_drawer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_drawer, BUTTON_GAP, 0);

    // Same size and style as STAND and SIT, so the drawer looks like more of
    // the same rail rather than a different control set.
    create_move_button(s_drawer, LV_SYMBOL_UP, Move::Up, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
    create_move_button(s_drawer, LV_SYMBOL_DOWN, Move::Down, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);

    for (int index = 0; index < 2; ++index) {
        char label[16];
        std::snprintf(label, sizeof(label), "PRESET %d", index + 1);
        lv_obj_t *btn = theme::make_button(s_drawer, label);
        lv_obj_set_size(btn, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
        lv_obj_add_event_cb(btn, preset_clicked_cb, LV_EVENT_SHORT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
        lv_obj_add_event_cb(btn, preset_clicked_cb, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
        register_desk_control(btn);
    }
}

// --- thermostat ------------------------------------------------------------

// The arc carries an integer, so everything is scaled to tenths of a degree
// and divided back out. Without that a 0.5 step is not representable.
constexpr int   DIAL_SCALE     = 10;
constexpr float DEFAULT_MIN_C  = 15.0f;
constexpr float DEFAULT_MAX_C  = 30.0f;
constexpr float DEFAULT_STEP_C = 0.5f;

constexpr std::int32_t DIAL_CARD_W = 480;
// The ring is this much narrower than the card, which leaves a square of empty
// space in the top corners. That is where the toggles go: on the card, clear of
// the arc, so a tap on one is never read as a drag of the setpoint.
constexpr std::int32_t DIAL_INSET    = 100;
constexpr std::int32_t DIAL_CHIP     = 56;
constexpr std::int32_t DIAL_CHIP_GAP = 10;

lv_obj_t *s_dial         = nullptr;
lv_obj_t *s_dial_current = nullptr;
lv_obj_t *s_dial_target  = nullptr;
lv_obj_t *s_dial_mode    = nullptr;
lv_obj_t *s_dial_toggles[kDialToggleCount] = {};

float s_dial_step = DEFAULT_STEP_C;
// Incoming updates are ignored while a finger is down, or the dial fights the
// drag and the setpoint jumps back under the thumb.
bool s_dial_dragging = false;

float dial_value_c()
{
    return static_cast<float>(lv_arc_get_value(s_dial)) / DIAL_SCALE;
}

/** Thermostats accept their own step only; anything else is rounded away. */
float snap(float celsius)
{
    if (s_dial_step <= 0.0f) {
        return celsius;
    }
    return std::round(celsius / s_dial_step) * s_dial_step;
}

void write_temperature(lv_obj_t *label, float celsius, bool with_unit)
{
    if (celsius < 0.0f) {
        theme::set_text(label, "--");
        return;
    }
    char      text[24];
    const int tenths = static_cast<int>(celsius * 10.0f + 0.5f);
    std::snprintf(text, sizeof(text), with_unit ? "%d.%d °C" : "%d.%d", tenths / 10, tenths % 10);
    theme::set_text(label, text);
}

void arc_changed_cb(lv_event_t *)
{
    s_dial_dragging = true;
    write_temperature(s_dial_target, snap(dial_value_c()), true);
}

void arc_released_cb(lv_event_t *)
{
    if (!s_dial_dragging) {
        return;
    }
    s_dial_dragging = false;
    // Sent on release rather than on every step of the drag: a thermostat does
    // not want thirty setpoints on the way to the one you meant.
    if (s_handlers.setpoint != nullptr) {
        s_handlers.setpoint(snap(dial_value_c()));
    }
}

void mode_clicked_cb(lv_event_t *)
{
    if (s_handlers.mode != nullptr) {
        s_handlers.mode();
    }
}

void dial_toggle_cb(lv_event_t *e)
{
    if (s_handlers.dial_toggle != nullptr) {
        s_handlers.dial_toggle(
            static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
    }
}

void build_thermostat(lv_obj_t *parent, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, w, h);
    theme::style_panel(card, theme::panel_light, 24);
    lv_obj_set_style_pad_all(card, PANEL_PAD, 0);

    const std::int32_t inner   = w - 2 * PANEL_PAD;
    const std::int32_t inner_h = h - 2 * PANEL_PAD;
    // Centred in the card, not in what is left under the toggles: reserving
    // space for those pushed the ring down and left it visibly low.
    const std::int32_t ring   = std::min(w - DIAL_INSET, inner_h);
    const std::int32_t ring_y = (inner_h - ring) / 2;

    s_dial = lv_arc_create(card);
    lv_obj_set_size(s_dial, ring, ring);
    lv_obj_align(s_dial, LV_ALIGN_TOP_MID, 0, ring_y);
    // A gap at the bottom, like a physical thermostat.
    lv_arc_set_bg_angles(s_dial, 135, 45);
    lv_arc_set_rotation(s_dial, 0);
    lv_arc_set_range(s_dial, static_cast<int>(DEFAULT_MIN_C * DIAL_SCALE),
                     static_cast<int>(DEFAULT_MAX_C * DIAL_SCALE));

    lv_obj_set_style_arc_width(s_dial, 24, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_dial, lv_color_hex(theme::panel), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_dial, 24, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_dial, lv_color_hex(theme::orange), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_dial, lv_color_hex(theme::orange), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_dial, 10, LV_PART_KNOB);

    lv_obj_add_event_cb(s_dial, arc_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_dial, arc_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_dial, arc_released_cb, LV_EVENT_PRESS_LOST, nullptr);

    // Against the ring's centre rather than the card's, so they stay put
    // whatever the card gains underneath them.
    const std::int32_t centre = ring_y + ring / 2;
    // Captioned: two temperatures stacked with nothing to tell them apart is
    // only readable once you already know which is which. Offsets put the
    // whole block symmetrically about the ring's centre.
    lv_obj_align(theme::make_label(card, "CURRENT", theme::secondary, fonts::size_16()),
                 LV_ALIGN_TOP_MID, 0, centre - 80);
    s_dial_current = theme::make_label(card, "--", theme::text, fonts::temp_64());
    lv_obj_align(s_dial_current, LV_ALIGN_TOP_MID, 0, centre - 58);
    lv_obj_align(theme::make_label(card, "TARGET", theme::secondary, fonts::size_16()),
                 LV_ALIGN_TOP_MID, 0, centre + 24);
    s_dial_target = theme::make_label(card, "--", theme::orange, fonts::temp_34());
    lv_obj_align(s_dial_target, LV_ALIGN_TOP_MID, 0, centre + 46);

    // After the arc, so these are on top of it and take their own presses.
    for (int i = 0; i < kDialToggleCount; ++i) {
        lv_obj_t *chip = theme::make_button(card, "", theme::panel, fonts::size_16());
        lv_obj_set_size(chip, DIAL_CHIP, DIAL_CHIP);
        lv_obj_set_style_radius(chip, DIAL_CHIP / 2, 0);
        lv_obj_set_pos(chip, inner - DIAL_CHIP - i * (DIAL_CHIP + DIAL_CHIP_GAP), 0);
        lv_obj_add_event_cb(chip, dial_toggle_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        s_dial_toggles[i] = chip;
        lv_obj_set_hidden(chip, true);
    }

    // Inside the ring's own gap rather than under the card. Kept narrow enough
    // that its top corners stay within the 90 degrees the arc does not draw,
    // and clear of where the knob parks at either end of the range.
    const std::int32_t mode_w = ring * 11 / 20;
    const std::int32_t mode_h = 68;
    s_dial_mode               = theme::make_button(card, "OFF", theme::panel);
    lv_obj_set_size(s_dial_mode, mode_w, mode_h);
    lv_obj_set_style_radius(s_dial_mode, mode_h / 2, 0);
    lv_obj_align(s_dial_mode, LV_ALIGN_TOP_MID, 0, ring_y + ring - mode_h + 4);
    lv_obj_add_event_cb(s_dial_mode, mode_clicked_cb, LV_EVENT_CLICKED, nullptr);
}

// --- sensor pills ----------------------------------------------------------

// Tall enough that the stacked label and value are not pressed against the
// rounded edge above and below them, without turning the strip into a band.
constexpr std::int32_t PILL_H = 60;
constexpr std::int32_t DOT    = 14;

struct Pill {
    lv_obj_t *root   = nullptr;
    lv_obj_t *dot    = nullptr;
    lv_obj_t *column = nullptr;
    lv_obj_t *label  = nullptr;
    lv_obj_t *value  = nullptr;
    bool      shown  = false;
};
Pill         s_pills[kPillCount];
std::int32_t s_pill_row_w = 0;

constexpr std::int32_t PILL_PAD = 22;

// Fixed widths, shared out across the strip. Sizing each pill to its own text
// made the whole row shuffle sideways every time a reading gained or lost a
// digit, which is exactly when you are looking at it.
void reflow_pills()
{
    int visible = 0;
    for (const Pill &pill : s_pills) {
        visible += pill.shown ? 1 : 0;
    }
    if (visible == 0) {
        return;
    }
    const std::int32_t w    = (s_pill_row_w - (visible - 1) * 12) / visible;
    const std::int32_t text = w - 2 * PILL_PAD - DOT - 10;
    for (const Pill &pill : s_pills) {
        if (!pill.shown) {
            continue;
        }
        lv_obj_set_width(pill.root, w);
        lv_obj_set_width(pill.column, text);
        lv_obj_set_width(pill.label, text);
        lv_obj_set_width(pill.value, text);
    }
}

std::uint32_t level_ink(Level level)
{
    switch (level) {
        case Level::Good: return theme::green;
        case Level::Warn: return theme::amber;
        case Level::Bad:  return theme::red;
        default:          return theme::secondary;
    }
}

// Readings only: nothing here is pressable, so they are rounded all the way
// and sit above the controls rather than among them.
void build_pills(lv_obj_t *parent, std::int32_t w)
{
    s_pill_row_w = w;

    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_size(strip, w, PILL_H);
    theme::style_panel(strip, theme::background, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(strip, 12, 0);

    for (int i = 0; i < kPillCount; ++i) {
        lv_obj_t *pill = lv_obj_create(strip);
        lv_obj_set_size(pill, s_pill_row_w / kPillCount, PILL_H);
        theme::style_panel(pill, theme::panel_light, PILL_H / 2);
        lv_obj_set_style_pad_hor(pill, PILL_PAD, 0);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(pill, 10, 0);

        // A reading on its own says nothing unless you already know what good
        // looks like. The dot is the whole point of showing it on a wall.
        lv_obj_t *dot = lv_obj_create(pill);
        lv_obj_set_size(dot, DOT, DOT);
        theme::style_panel(dot, theme::secondary, DOT / 2);
        lv_obj_set_clickable(dot, false);

        // Stacked rather than side by side: four readings across one strip do
        // not fit on a single line once each carries a name and a unit.
        lv_obj_t *column = lv_obj_create(pill);
        lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(column, 1);
        theme::style_panel(column, theme::panel_light, 0);
        lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
        lv_obj_set_clickable(column, false);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        // Pulls the name and the reading together without touching the pill's
        // own size, so the outer spacing stays as it is. The labels are all
        // upper case, so the tightened rows have no descenders to clip.
        lv_obj_set_style_pad_row(column, -2, 0);

        lv_obj_t *label = theme::make_label(column, "", theme::secondary, fonts::size_16());
        lv_obj_t *value = theme::make_label(column, "", theme::text, fonts::size_22());
        // Dots rather than a pill that grows: the safe area is the promise.
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);

        s_pills[i] = Pill{pill, dot, column, label, value, false};
        lv_obj_set_hidden(pill, true);
    }
}

// --- lights ----------------------------------------------------------------

constexpr std::int32_t BULB_W = 34;
constexpr std::int32_t BULB_H = 48;

lv_obj_t *s_lights_button = nullptr;
lv_obj_t *s_lights_name   = nullptr;
lv_obj_t *s_lights_state  = nullptr;
lv_obj_t *s_bulbs[kLightCount]   = {};
bool      s_light_on[kLightCount] = {};
bool      s_lights_on             = false;
// A long press fires LONG_PRESSED and then CLICKED on release. Without this the
// picker would open and the lights would toggle from the same gesture.
bool s_lights_long = false;

std::optional<ModalOverlay> s_light_picker;
struct LightButton {
    lv_obj_t *root  = nullptr;
    lv_obj_t *name  = nullptr;
    lv_obj_t *state = nullptr;
};
LightButton s_lights[kLightCount];

// A bulb from a round glass and a flat base. At this size anything more
// detailed reads as a smudge.
lv_obj_t *make_bulb(lv_obj_t *parent)
{
    lv_obj_t *bulb = lv_obj_create(parent);
    lv_obj_set_size(bulb, BULB_W, BULB_H);
    theme::style_panel(bulb, theme::panel, 0);
    lv_obj_set_style_bg_opa(bulb, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(bulb, false);

    lv_obj_t *glass = lv_obj_create(bulb);
    lv_obj_set_size(glass, BULB_W, BULB_W);
    lv_obj_set_pos(glass, 0, 0);
    theme::style_panel(glass, theme::disabled_ink, BULB_W / 2);
    lv_obj_set_clickable(glass, false);

    lv_obj_t *base = lv_obj_create(bulb);
    lv_obj_set_size(base, 16, BULB_H - BULB_W - 3);
    lv_obj_set_pos(base, (BULB_W - 16) / 2, BULB_W + 3);
    theme::style_panel(base, theme::disabled_ink, 3);
    lv_obj_set_clickable(base, false);
    return bulb;
}

// Which lamps are on, drawn on the button that controls all of them. The pair
// of colours flips with the button's own fill, or a lit bulb would be orange
// on orange the moment everything is on.
void paint_bulbs()
{
    const std::uint32_t lit   = s_lights_on ? theme::background : theme::orange;
    const std::uint32_t unlit = s_lights_on ? theme::orange_dim : theme::disabled_ink;
    for (int i = 0; i < kLightCount; ++i) {
        if (s_bulbs[i] == nullptr) {
            continue;
        }
        const std::uint32_t ink = s_light_on[i] ? lit : unlit;
        theme::set_bg_color(lv_obj_get_child(s_bulbs[i], 0), ink);
        theme::set_bg_color(lv_obj_get_child(s_bulbs[i], 1), ink);
    }
}

void paint_light(lv_obj_t *root, lv_obj_t *name, lv_obj_t *state, bool on)
{
    theme::set_bg_color(root, on ? theme::orange : theme::panel_light);
    theme::set_text_color(name, on ? theme::background : theme::secondary);
    theme::set_text_color(state, on ? theme::background : theme::text);
}

void lights_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        s_lights_long = true;
        if (s_light_picker.has_value()) {
            s_light_picker->open(s_lights_button);
        }
        return;
    }
    if (std::exchange(s_lights_long, false)) {
        return;
    }
    if (s_handlers.lights != nullptr) {
        s_handlers.lights();
    }
}

void light_clicked_cb(lv_event_t *e)
{
    if (s_handlers.light != nullptr) {
        s_handlers.light(
            static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
    }
}

void build_lights_button(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                         std::int32_t h)
{
    s_lights_button = lv_button_create(parent);
    lv_obj_set_pos(s_lights_button, x, y);
    lv_obj_set_size(s_lights_button, w, h);
    theme::style_button(s_lights_button, theme::panel_light);
    lv_obj_set_style_pad_all(s_lights_button, 28, 0);
    lv_obj_add_event_cb(s_lights_button, lights_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_lights_button, lights_event_cb, LV_EVENT_LONG_PRESSED, nullptr);

    s_lights_name = theme::make_label(s_lights_button, "LIGHTS", theme::secondary,
                                      fonts::size_22());
    lv_obj_align(s_lights_name, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lights_state = theme::make_label(s_lights_button, "--", theme::text, fonts::size_48());
    lv_obj_align(s_lights_state, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *strip = lv_obj_create(s_lights_button);
    lv_obj_set_size(strip, LV_SIZE_CONTENT, BULB_H);
    theme::style_panel(strip, theme::panel, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(strip, false);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(strip, 14, 0);

    for (int i = 0; i < kLightCount; ++i) {
        s_bulbs[i] = make_bulb(strip);
        lv_obj_set_hidden(s_bulbs[i], true);
    }
}

// Full-size buttons in a grid rather than the usual list of thin rows: this is
// reached for across a room, not read from a desk chair.
void build_light_picker(lv_obj_t *parent)
{
    constexpr std::int32_t COLUMNS  = 2;
    constexpr std::int32_t BTN_W    = 272;
    constexpr std::int32_t BTN_H    = 170;
    constexpr std::int32_t PAD      = 24;
    const std::int32_t     rows     = (kLightCount + COLUMNS - 1) / COLUMNS;
    const std::int32_t     card_w   = COLUMNS * BTN_W + (COLUMNS - 1) * BUTTON_GAP + 2 * PAD;
    const std::int32_t     header_h = 52;
    const std::int32_t card_h = rows * BTN_H + (rows - 1) * BUTTON_GAP + header_h + 2 * PAD;

    s_light_picker.emplace(parent, card_w, card_h);
    lv_obj_t *card = s_light_picker->content();
    lv_obj_set_style_pad_all(card, PAD, 0);

    lv_obj_t *title = theme::make_label(card, "LIGHTS", theme::orange, fonts::size_22());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_pos(grid, 0, header_h);
    lv_obj_set_size(grid, card_w - 2 * PAD, card_h - 2 * PAD - header_h);
    theme::style_panel(grid, theme::panel, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, BUTTON_GAP, 0);
    lv_obj_set_style_pad_column(grid, BUTTON_GAP, 0);

    for (int i = 0; i < kLightCount; ++i) {
        lv_obj_t *btn = lv_button_create(grid);
        lv_obj_set_size(btn, BTN_W, BTN_H);
        theme::style_button(btn, theme::panel_light);
        lv_obj_set_style_pad_all(btn, 20, 0);
        lv_obj_add_event_cb(btn, light_clicked_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));

        lv_obj_t *name = theme::make_label(btn, "", theme::secondary, fonts::size_20());
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_width(name, BTN_W - 40);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

        lv_obj_t *state = theme::make_label(btn, "--", theme::text, fonts::size_32());
        lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        s_lights[i] = LightButton{btn, name, state};
        lv_obj_set_hidden(btn, true);
    }
}

// --- home screen tiles -----------------------------------------------------

struct Tile {
    lv_obj_t *root  = nullptr;
    lv_obj_t *label = nullptr;
    lv_obj_t *value = nullptr;
    bool      shown = false;
};
Tile         s_tiles[kTileCount];
std::int32_t s_tile_row_w = 0;

// The tiles share one row, so their width depends on how many are in use.
// Fixed widths would leave a single tile sitting in a quarter of the space.
void reflow_tiles()
{
    int visible = 0;
    for (const Tile &tile : s_tiles) {
        visible += tile.shown ? 1 : 0;
    }
    if (visible == 0) {
        return;
    }
    const std::int32_t w = (s_tile_row_w - (visible - 1) * BUTTON_GAP) / visible;
    for (const Tile &tile : s_tiles) {
        if (!tile.shown) {
            continue;
        }
        lv_obj_set_width(tile.root, w);
        lv_obj_set_width(tile.label, w - 28);
        lv_obj_set_width(tile.value, w - 28);
    }
}

void tile_clicked_cb(lv_event_t *e)
{
    if (s_handlers.tile != nullptr) {
        s_handlers.tile(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
    }
}

void build_home_page(lv_obj_t *page)
{
    const Layout l = layout();

    lv_obj_set_style_pad_all(page, PANEL_PAD, 0);
    const std::int32_t inner_w = l.content_w - 2 * PANEL_PAD;
    const std::int32_t inner_h = l.content_h - 2 * PANEL_PAD;

    build_pills(page, inner_w);

    const std::int32_t body_y = PILL_H + BUTTON_GAP;
    const std::int32_t body_h = inner_h - body_y;
    build_thermostat(page, body_y, DIAL_CARD_W, body_h);

    // Everything else lives in the column beside the dial. Letting it wrap
    // across the whole page is what put the tiles on top of the thermostat.
    const std::int32_t col_x    = DIAL_CARD_W + BUTTON_GAP;
    const std::int32_t col_w    = inner_w - col_x;
    const std::int32_t lights_h = body_h * 5 / 9;
    build_lights_button(page, col_x, body_y, col_w, lights_h);

    const std::int32_t tiles_y = body_y + lights_h + BUTTON_GAP;
    const std::int32_t tiles_h = inner_h - tiles_y;
    s_tile_row_w               = col_w;

    lv_obj_t *row = lv_obj_create(page);
    lv_obj_set_pos(row, col_x, tiles_y);
    lv_obj_set_size(row, col_w, tiles_h);
    theme::style_panel(row, theme::background, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, BUTTON_GAP, 0);

    for (int i = 0; i < kTileCount; ++i) {
        lv_obj_t *tile = lv_button_create(row);
        lv_obj_set_size(tile, col_w, tiles_h);
        theme::style_button(tile, theme::panel_light);
        lv_obj_set_style_pad_all(tile, 18, 0);
        lv_obj_add_event_cb(tile, tile_clicked_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));

        lv_obj_t *label = theme::make_label(tile, "", theme::secondary, fonts::size_16());
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);

        lv_obj_t *value = theme::make_label(tile, "", theme::text, fonts::size_28());
        lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);

        s_tiles[i] = Tile{tile, label, value, false};
        lv_obj_set_hidden(tile, true);
    }

    // Last, so the overlay covers the page rather than being covered by it.
    build_light_picker(page);
}

// --- pages and navigation --------------------------------------------------

constexpr int PAGE_COUNT = 5;
lv_obj_t *s_pages[PAGE_COUNT]    = {};
lv_obj_t *s_nav_tabs[PAGE_COUNT] = {};

// Icon plus a short caption: icons alone are a guessing game, and full words
// eat the width we want for more destinations later.
struct NavItem {
    const char *icon;
    const char *caption;
    // Hidden from the navigation while the tracked phone is away. Home stays
    // because the desk has to be usable by anyone, and Setup stays because it
    // is where the presence readout lives -- gating that would hide the one
    // screen that explains why everything else vanished.
    bool        needs_presence;
};
constexpr NavItem NAV_ITEMS[PAGE_COUNT] = {
    {LV_SYMBOL_HOME, "Home", false},    {LV_SYMBOL_LIST, "Stats", true},
    {LV_SYMBOL_BELL, "Alerts", true},   {LV_SYMBOL_WIFI, "Network", true},
    {LV_SYMBOL_SETTINGS, "Setup", false},
};

// Nothing is hidden until the phone has actually been recognised once. A panel
// that hides half its pages because the key is wrong, or because Bluetooth
// never came up, is worse than one that does not gate at all.
bool s_presence_known = false;
bool s_present        = false;
int  s_page           = 0;

bool page_available(int index)
{
    return !NAV_ITEMS[index].needs_presence || !s_presence_known || s_present;
}

void select_page(int index)
{
    if (!page_available(index)) {
        index = 0;
    }
    s_page = index;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_set_hidden(s_nav_tabs[i], !page_available(i));
        lv_obj_set_hidden(s_pages[i], i != index);
        const bool          active = (i == index);
        lv_obj_set_style_bg_color(s_nav_tabs[i],
                                  lv_color_hex(active ? theme::orange : theme::panel_light), 0);
        const std::uint32_t ink = active ? theme::background : theme::secondary;
        theme::set_text_color(lv_obj_get_child(s_nav_tabs[i], 0), ink);
        theme::set_text_color(lv_obj_get_child(s_nav_tabs[i], 1), ink);
    }
}

void nav_event_cb(lv_event_t *e)
{
    select_page(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
}

void brightness_event_cb(lv_event_t *e)
{
    if (s_handlers.brightness == nullptr) {
        return;
    }
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    s_handlers.brightness(static_cast<int>(lv_slider_get_value(slider)));
}

// Stand-ins, so the navigation can be seen working before the screens behind
// it exist. Each says what it is for rather than pretending to hold data.
void build_placeholder_page(lv_obj_t *page, const char *title, const char *blurb)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 14, 0);
    theme::make_label(page, title, theme::text, fonts::size_32());
    theme::make_label(page, blurb, theme::secondary, fonts::size_20());
}

void build_settings_page(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 20, 0);

    lv_obj_t *caption = lv_label_create(page);
    lv_label_set_text_static(caption, "Brightness");
    lv_obj_set_style_text_color(caption, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_text_font(caption, fonts::size_28(), 0);

    lv_obj_t *slider = lv_slider_create(page);
    lv_obj_set_width(slider, 480);
    // Starts at the lowest the panel honours: a slider whose bottom third does
    // nothing reads as broken.
    lv_slider_set_range(slider, board::kMinBrightness, 100);
    lv_slider_set_value(slider, s_initial_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme::panel_light), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme::orange), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme::orange), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *hint = lv_label_create(page);
    lv_label_set_text_static(hint, "Tap a preset to go there, hold to save it");
    lv_obj_set_style_text_color(hint, lv_color_hex(theme::secondary), 0);

}

void create_content(lv_obj_t *parent)
{


    const Layout l = layout();

    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_set_pos(area, l.content_x, GAP);
    lv_obj_set_size(area, l.content_w, l.screen_h - GAP - EDGE_GAP);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_scrollable(area, false);

    lv_obj_t *nav = lv_obj_create(area);
    lv_obj_set_size(nav, l.content_w, NAV_H);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav, 0, 0);
    lv_obj_set_style_pad_all(nav, 0, 0);
    lv_obj_set_scrollable(nav, false);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, BUTTON_GAP, 0);

    const std::int32_t tab_w =
        (l.content_w - (PAGE_COUNT - 1) * BUTTON_GAP) / PAGE_COUNT;

    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_t *tab = lv_button_create(nav);
        lv_obj_set_size(tab, tab_w, NAV_H - 2 * (PANEL_PAD / 2));
        theme::style_button(tab, theme::panel_light);
        lv_obj_add_event_cb(tab, nav_event_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));

        lv_obj_t *icon = theme::make_label(tab, NAV_ITEMS[i].icon, theme::secondary,
                                           fonts::size_28());
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -12);
        lv_obj_t *caption = theme::make_label(tab, NAV_ITEMS[i].caption, theme::secondary,
                                              fonts::size_16());
        lv_obj_align(caption, LV_ALIGN_CENTER, 0, 16);
        s_nav_tabs[i] = tab;

        lv_obj_t *page = lv_obj_create(area);
        lv_obj_set_size(page, l.content_w, l.content_h);
        lv_obj_set_align(page, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(page, 0, 0);
        theme::style_panel(page);
        lv_obj_set_style_pad_all(page, 20, 0);
        s_pages[i] = page;
    }

    build_home_page(s_pages[0]);
    build_placeholder_page(s_pages[1], "Stats", "Height over time, hours stood, that sort of thing.");
    build_placeholder_page(s_pages[2], "Alerts", "Reminders to stand, and whatever Home Assistant sends.");
    build_placeholder_page(s_pages[3], "Network", "Wi-Fi and broker detail when something is wrong.");
    build_settings_page(s_pages[4]);
    select_page(0);
}

void build_screen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(theme::background), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

    create_rail(scr);
    create_content(scr);
    create_drawer(scr);  // after the content, so it overlays it when open
    create_notice_card();
}

}  // namespace

esp_err_t init(const Handlers &handlers, int initial_brightness)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    // Before any label exists: the fonts carry the fallback that supplies the
    // characters Montserrat does not have.
    fonts::init();
    s_handlers           = handlers;
    s_initial_brightness = initial_brightness;
    build_screen();
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_height(int height_mm)
{
    ESP_RETURN_ON_FALSE(s_height.has_value(), ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    s_height->set_tenths(height_mm, theme::orange);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_desk_available(bool available)
{
    ESP_RETURN_ON_FALSE(s_desk_control_count > 0, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    s_desk_available = available;
    for (int i = 0; i < s_desk_control_count; ++i) {
        lv_obj_t *obj = s_desk_controls[i];
        // The state drives the styling, so the button keeps a background and
        // simply changes colour. Labels inherit the state's text colour, but
        // only if they have not been given one of their own.
        lv_obj_set_state(obj, LV_STATE_DISABLED, !available);
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        if (label != nullptr) {
            theme::set_text_color(label, available ? theme::text : theme::disabled_ink);
        }
        // Not merely dimmed: a press that cannot reach the desk should do
        // nothing rather than appear to work.
        lv_obj_set_clickable(obj, available);
    }
    // The readout is not touched here: what it shows is the caller's business,
    // and blanking it from two places is how it ended up stuck empty.
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_tile(int index, const char *label, const char *value, bool on, bool actionable)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kTileCount, ESP_ERR_INVALID_ARG, TAG, "tile %d",
                        index);
    ESP_RETURN_ON_FALSE(s_tiles[index].root != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    Tile      &tile  = s_tiles[index];
    const bool empty = label == nullptr || label[0] == '\0';
    lv_obj_set_hidden(tile.root, empty);
    if (tile.shown == empty) {
        tile.shown = !empty;
        reflow_tiles();
    }
    if (!empty) {
        theme::set_text(tile.label, label);
        theme::set_text(tile.value, value != nullptr ? value : "");
        // Tint rather than a separate indicator: at a glance across a room the
        // colour is what reads, not a small dot.
        theme::set_bg_color(tile.root, on ? theme::orange : theme::panel_light);
        theme::set_text_color(tile.value, on ? theme::background : theme::text);
        theme::set_text_color(tile.label, on ? theme::background : theme::secondary);
        lv_obj_set_clickable(tile.root, actionable);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_pill(int index, const char *label, const char *value, Level level)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kPillCount, ESP_ERR_INVALID_ARG, TAG, "pill %d",
                        index);
    ESP_RETURN_ON_FALSE(s_pills[index].root != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    Pill      &pill  = s_pills[index];
    const bool empty = label == nullptr || label[0] == '\0';
    lv_obj_set_hidden(pill.root, empty);
    if (pill.shown == empty) {
        pill.shown = !empty;
        reflow_pills();
    }
    if (!empty) {
        theme::set_text(pill.label, label);
        theme::set_text(pill.value, value != nullptr ? value : "--");
        theme::set_bg_color(pill.dot, level_ink(level));
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_lights(const char *label, const char *state, bool on)
{
    ESP_RETURN_ON_FALSE(s_lights_button != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    theme::set_text(s_lights_name, label != nullptr ? label : "LIGHTS");
    theme::set_text(s_lights_state, state != nullptr ? state : "--");
    paint_light(s_lights_button, s_lights_name, s_lights_state, on);
    s_lights_on = on;
    paint_bulbs();
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_light(int index, const char *name, const char *state, bool on)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kLightCount, ESP_ERR_INVALID_ARG, TAG, "light %d",
                        index);
    ESP_RETURN_ON_FALSE(s_lights[index].root != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    LightButton &light = s_lights[index];
    const bool   empty = name == nullptr || name[0] == '\0';
    lv_obj_set_hidden(light.root, empty);
    lv_obj_set_hidden(s_bulbs[index], empty);
    if (!empty) {
        theme::set_text(light.name, name);
        theme::set_text(light.state, state != nullptr ? state : "--");
        paint_light(light.root, light.name, light.state, on);
    }
    s_light_on[index] = !empty && on;
    paint_bulbs();
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_dial_toggle(int index, const char *label, bool on)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kDialToggleCount, ESP_ERR_INVALID_ARG, TAG,
                        "toggle %d", index);
    ESP_RETURN_ON_FALSE(s_dial_toggles[index] != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    lv_obj_t  *chip  = s_dial_toggles[index];
    const bool empty = label == nullptr || label[0] == '\0';
    lv_obj_set_hidden(chip, empty);
    if (!empty) {
        lv_obj_t *text = lv_obj_get_child(chip, 0);
        theme::set_text(text, label);
        theme::set_bg_color(chip, on ? theme::orange : theme::panel);
        theme::set_text_color(text, on ? theme::background : theme::secondary);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_thermostat_range(float min_c, float max_c, float step_c)
{
    ESP_RETURN_ON_FALSE(s_dial != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    // Bounds come from the entity, so a thermostat reconfigured in Home
    // Assistant is followed rather than clamped to something stale.
    lv_arc_set_range(s_dial, static_cast<int>(min_c * DIAL_SCALE),
                     static_cast<int>(max_c * DIAL_SCALE));
    s_dial_step = step_c > 0.0f ? step_c : DEFAULT_STEP_C;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_thermostat(float current_c, float target_c, const char *mode, Hvac state)
{
    ESP_RETURN_ON_FALSE(s_dial != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    write_temperature(s_dial_current, current_c, true);
    if (!s_dial_dragging) {
        write_temperature(s_dial_target, target_c, true);
        if (target_c >= 0.0f) {
            lv_arc_set_value(s_dial, static_cast<int>(target_c * DIAL_SCALE + 0.5f));
        }
    }

    // The ring carries the state: the same orange every other live control on
    // the panel uses while the boiler runs, amber when it is on but coasting,
    // grey when it is off altogether.
    std::uint32_t ink = theme::secondary;
    switch (state) {
        case Hvac::Heating: ink = theme::orange; break;
        case Hvac::Idle:    ink = theme::amber;  break;
        case Hvac::Off:     break;
    }
    theme::set_arc_color(s_dial, ink, LV_PART_INDICATOR);
    theme::set_bg_color(s_dial, ink, LV_PART_KNOB);

    // And the switch reads like every other thing that is on.
    const bool on = state != Hvac::Off;
    theme::set_bg_color(s_dial_mode, on ? theme::orange : theme::panel);
    lv_obj_t *mode_text = lv_obj_get_child(s_dial_mode, 0);
    theme::set_text(mode_text, mode != nullptr ? mode : "--");
    theme::set_text_color(mode_text, on ? theme::background : theme::text);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_presence(bool has_key, bool present, bool ever_seen)
{
    ESP_RETURN_ON_FALSE(s_phone_slash != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    const bool here = has_key && present;
    lv_obj_set_hidden(s_phone_slash, here);

    // Only gate pages once the phone has actually been recognised at least
    // once this boot. Hiding half the panel because a key is wrong, or because
    // Bluetooth never came up, is worse than not gating at all.
    const bool known = has_key && ever_seen;
    if (known != s_presence_known || here != s_present) {
        s_presence_known = known;
        s_present        = here;
        // Falls back to Home if the page in front of you just went away.
        select_page(s_page);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_time(const char *text)
{
    ESP_RETURN_ON_FALSE(s_clock_label != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    static char last[8] = {};
    if (text != nullptr && std::strncmp(text, last, sizeof(last)) == 0) {
        return ESP_OK;
    }
    std::snprintf(last, sizeof(last), "%s", text != nullptr ? text : "");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_label_set_text(s_clock_label, text != nullptr ? text : "--:--");
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_links(bool wifi, bool mqtt)
{
    (void)mqtt;  // the broker has its own indicator in Home Assistant
    ESP_RETURN_ON_FALSE(s_wifi_icon != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    // Called on a timer, so it is almost always a no-op. Restyling anyway
    // would invalidate the icon every couple of seconds for nothing.
    static int last = -1;
    if (last == (wifi ? 1 : 0)) {
        return ESP_OK;
    }
    last = wifi ? 1 : 0;
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    // White either way; the slash is what says "not connected".
    lv_obj_set_hidden(s_wifi_slash, wifi);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_battery(bool present, int percent, bool charging)
{
    // Nothing on screen shows battery any more -- it lives in Home Assistant.
    // Kept so callers need not care, and so re-adding an indicator is local.
    (void)present;
    (void)percent;
    (void)charging;
    return ESP_OK;
}

esp_err_t notify(const char *title, const char *message, const char *level, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_notice_card != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    if (s_notice_count == NOTIFY_QUEUE_LEN) {
        // Drop the oldest and say so, rather than losing one silently.
        ESP_LOGW(TAG, "notification queue full, dropping oldest");
        for (int i = 1; i < NOTIFY_QUEUE_LEN; ++i) {
            s_notice_queue[i - 1] = s_notice_queue[i];
        }
        --s_notice_count;
    }

    Notice &slot = s_notice_queue[s_notice_count++];
    std::snprintf(slot.title, sizeof(slot.title), "%s", title != nullptr ? title : "");
    std::snprintf(slot.message, sizeof(slot.message), "%s", message != nullptr ? message : "");
    std::snprintf(slot.level, sizeof(slot.level), "%s", level != nullptr ? level : "info");
    slot.timeout_ms = timeout_ms;

    if (lv_obj_is_hidden(s_notice_card)) {
        show_next_notice();
    }
    lvgl_port_unlock();
    return ESP_OK;
}

}  // namespace ui
