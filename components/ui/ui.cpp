#include "ui.h"

#include "board.h"
#include "media.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "modal_overlay.h"
#include "radar_page.h"
#include "segment_display.h"
#include "theme.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>

namespace ui {
namespace {

constexpr char TAG[] = "ui";

// Generous: at 100 ms a push landing during the first full-screen render simply gave up.
constexpr std::uint32_t LOCK_TIMEOUT_MS = 500;

// Sized from the real display rather than with percentages: LV_PCT() returns an encoded
// sentinel, so LV_PCT(100) - something is not a width, it is nonsense that lays out wrong.
constexpr std::int32_t RAIL_W      = 330;
constexpr std::int32_t NAV_H       = 92;
constexpr std::int32_t RAIL_BTN_H  = 124;
constexpr std::int32_t GAP        = 16;
constexpr std::int32_t EDGE_GAP   = 0;   // navigation sits on the bottom edge
// Matches the gap above the page, so the content sits centred rather than crowding the buttons.
constexpr std::int32_t NAV_GAP    = GAP;

constexpr std::int32_t PANEL_PAD  = 16;
constexpr std::int32_t BUTTON_GAP = 16;

struct Layout {
    std::int32_t screen_w;
    std::int32_t screen_h;
    std::int32_t content_x;
    std::int32_t content_w;
    std::int32_t content_h;
    std::int32_t rail_x;
    bool         rail_right;
};

// Which edge the rail is against. Everything placed against it asks the layout
// rather than assuming the left.
bool s_rail_right = false;

// Which way up the panel is hung. The picture and the touchscreen are turned
// together by board::set_flipped(); this is only what the page shows.
bool s_flipped = false;

Layout layout()
{
    lv_display_t      *disp = lv_display_get_default();
    const std::int32_t w    = lv_display_get_horizontal_resolution(disp);
    const std::int32_t h    = lv_display_get_vertical_resolution(disp);
    const std::int32_t x      = s_rail_right ? GAP : RAIL_W + GAP;
    const std::int32_t area_h = h - GAP - EDGE_GAP;
    return Layout{w,
                  h,
                  x,
                  w - RAIL_W - 2 * GAP,
                  area_h - NAV_H - NAV_GAP,
                  s_rail_right ? w - RAIL_W : 0,
                  s_rail_right};
}

Handlers s_handlers{};

// Dimmed as a group when the control box is not answering.
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
lv_obj_t *s_rail          = nullptr;
lv_obj_t *s_content       = nullptr;
lv_obj_t *s_clock_box     = nullptr;
lv_obj_t *s_side_buttons[2] = {};
lv_obj_t *s_flip_buttons[2] = {};
lv_obj_t *s_wifi_icon     = nullptr;
lv_obj_t *s_wifi_slash    = nullptr;
lv_obj_t *s_phone_icon    = nullptr;
lv_obj_t *s_phone_slash   = nullptr;
lv_obj_t *s_clock_hours   = nullptr;
lv_obj_t *s_clock_colon   = nullptr;
lv_obj_t *s_clock_minutes = nullptr;
bool      s_clock_known   = false;

void clock_blink(lv_timer_t *)
{
    if (!s_clock_known) {
        return;
    }
    const lv_opa_t now = lv_obj_get_style_opa(s_clock_colon, LV_PART_MAIN);
    lv_obj_set_style_opa(s_clock_colon, now == LV_OPA_COVER ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

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

struct NoticeInk {
    std::uint32_t colour;
    bool          accent;
};

NoticeInk notice_ink(const char *level)
{
    if (std::strcmp(level, "error") == 0) return {theme::red, false};
    if (std::strcmp(level, "warning") == 0) return {theme::amber, false};
    if (std::strcmp(level, "success") == 0) return {theme::green, false};
    return {theme::primary, true};
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

    const NoticeInk ink = notice_ink(notice.level);
    theme::fill_accent_or(s_notice_bar, ink.accent, ink.colour);
    // A message with no title reads better promoted into the heading.
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

void create_notice_card()
{
    const Layout l = layout();

    // Covers the page and the navigation under it, so there is no navigating out from under a
    // notification. The rail stays live, so the desk can still be driven while a message is up.
    s_notice_scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_notice_scrim, l.rail_right ? 0 : RAIL_W, 0);
    lv_obj_set_size(s_notice_scrim, l.screen_w - RAIL_W, l.screen_h);
    theme::style_panel(s_notice_scrim, theme::background, 0);
    lv_obj_set_style_bg_opa(s_notice_scrim, LV_OPA_70, 0);
    lv_obj_set_hidden(s_notice_scrim, true);
    lv_obj_set_clickable(s_notice_scrim, true);
    lv_obj_add_event_cb(s_notice_scrim, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);

    s_notice_card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_notice_card, NOTIFY_W, NOTIFY_H);
    // On the top layer so no page can cover it, but centred over the content area rather than
    // the screen, which would put it half behind the rail.
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

void move_event_cb(lv_event_t *e)
{
    if (s_handlers.move == nullptr) {
        return;
    }
    const auto direction =
        static_cast<Move>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_handlers.move(lv_event_get_code(e) == LV_EVENT_PRESSED ? direction : Move::Stop);
}

// PRESS_LOST matters as much as RELEASED: a finger sliding off must not leave the desk moving.
void create_move_button(lv_obj_t *parent, const char *symbol, Move direction, std::int32_t w,
                        std::int32_t h)
{
    // Via the shared helper: styling by hand left LVGL's default shadow showing as a ring.
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
    theme::style_panel(bar, theme::panel, 2);
    theme::fill_accent(bar);
    // White while the preset it belongs to is the one the desk stands at, the
    // same way it goes white under a finger.
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::text), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::text), LV_STATE_PRESSED);
    lv_obj_set_clickable(bar, false);
    return bar;
}

// Five bars: top, two legs, two feet. The legs get shorter for the sit variant.
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

// The clock keeps the corner furthest from the content, so moving the rail
// mirrors the strip rather than leaving the time stranded in the middle.
void place_strip()
{
    const bool right = s_rail_right;
    theme::align(s_clock_box, right ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    theme::align(s_wifi_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    theme::align(s_phone_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, right ? 44 : -44,
                 0);
    lv_obj_align_to(s_wifi_slash, s_wifi_icon, LV_ALIGN_CENTER, 0, -2);
    lv_obj_align_to(s_phone_slash, s_phone_icon, LV_ALIGN_CENTER, 0, -2);
}

lv_obj_t *make_rail_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = theme::make_button(parent, text);
    lv_obj_set_size(btn, RAIL_W - 2 * PANEL_PAD, RAIL_BTN_H);
    theme::fill_accent(btn, LV_STATE_CHECKED);
    register_desk_control(btn);
    return btn;
}

void preset_clicked_cb(lv_event_t *e);

lv_obj_t *s_preset_buttons[kPresetCount] = {};
bool      s_preset_active[kPresetCount]  = {};

void bind_preset(lv_obj_t *button, int index)
{
    s_preset_buttons[index] = button;
    lv_obj_add_event_cb(button, preset_clicked_cb, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_add_event_cb(button, preset_clicked_cb, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
}

void preset_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    const bool store = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    // Already there: travelling to where you are is nothing but a jolt. Storing
    // still works, since that is about this height, not about moving to it.
    if (!store && index >= 0 && index < kPresetCount && s_preset_active[index]) {
        return;
    }
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
    s_rail         = rail;
    lv_obj_set_pos(rail, l.rail_x, 0);
    lv_obj_set_size(rail, RAIL_W, l.screen_h);
    theme::style_panel(rail);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_set_style_pad_all(rail, PANEL_PAD, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rail, BUTTON_GAP, 0);

    lv_obj_t *strip = lv_obj_create(rail);
    lv_obj_set_size(strip, RAIL_W - 2 * PANEL_PAD, 44);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_scrollable(strip, false);

    // Three labels in a row rather than one string: blinking the separator by
    // swapping it for a space would reflow the digits, because the glyphs are
    // not the same width. Fading it in place leaves them still.
    lv_obj_t *clock = lv_obj_create(strip);
    s_clock_box     = clock;
    lv_obj_set_size(clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    theme::style_panel(clock, theme::panel, 0);
    lv_obj_set_style_bg_opa(clock, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(clock, false);
    lv_obj_set_flex_flow(clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock, 0, 0);

    s_clock_hours   = theme::make_label(clock, "--", theme::text, fonts::size_28());
    s_clock_colon   = theme::make_label(clock, ":", theme::text, fonts::size_28());
    // The colon's own side bearings are not symmetric, so the gap is set on it
    // rather than on the row.
    // The colon glyph carries more bearing on its right than its left, so equal
    // padding does not look equal; the hours need the wider gap to balance it.
    lv_obj_set_style_pad_left(s_clock_colon, 4, 0);
    lv_obj_set_style_pad_right(s_clock_colon, 4, 0);
    s_clock_minutes = theme::make_label(clock, "--", theme::text, fonts::size_28());
    lv_timer_create(clock_blink, 1000, nullptr);

    s_wifi_icon = theme::make_label(strip, LV_SYMBOL_WIFI, theme::text, fonts::size_22());

    // A slash over the glyph: LVGL has no wifi-off symbol.
    s_wifi_slash = theme::make_label(strip, "/", theme::text, fonts::size_32());

    // Drawn rather than taken from the font: LVGL's phone symbol is a corded handset. The colour
    // never changes -- the slash is the state.
    s_phone_icon = lv_obj_create(strip);
    lv_obj_set_size(s_phone_icon, 18, 26);
    theme::style_panel(s_phone_icon, theme::text, 4);
    lv_obj_set_clickable(s_phone_icon, false);

    lv_obj_t *phone_screen = lv_obj_create(s_phone_icon);
    lv_obj_set_size(phone_screen, 13, 18);
    lv_obj_align(phone_screen, LV_ALIGN_TOP_MID, 0, 3);
    theme::style_panel(phone_screen, theme::panel, 2);
    lv_obj_set_clickable(phone_screen, false);

    s_phone_slash = theme::make_label(strip, "/", theme::text, fonts::size_32());
    place_strip();

    // The status strip stays where it was, under the bezel; everything below it
    // reads better carried a little further down.
    lv_obj_t *heading = theme::make_label(rail, "DESK HEIGHT", theme::secondary,
                                          &lv_font_montserrat_18);
    lv_obj_set_style_margin_top(heading, 18, 0);

    lv_obj_t *readout = lv_obj_create(rail);
    lv_obj_set_size(readout, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    theme::style_panel(readout, theme::panel, 0);
    lv_obj_set_style_bg_opa(readout, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(readout, LV_FLEX_FLOW_COLUMN);
    // END on the cross axis right-aligns the unit under the digits rather than beside them.
    lv_obj_set_flex_align(readout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_bottom(readout, 18, 0);

    s_height.emplace(readout);
    s_height->set_tenths(-1);

    theme::make_label(readout, "CM", theme::secondary, fonts::size_22());

    // Presets 3 and 4 are the two anyone uses, so they get names and the space; the rest
    // live behind Manual.
    lv_obj_t *stand = make_rail_button(rail, "STAND");
    add_desk_icon(stand, true);
    lv_obj_align(lv_obj_get_child(stand, 0), LV_ALIGN_CENTER, 34, 0);
    bind_preset(stand, 2);

    lv_obj_t *sit = make_rail_button(rail, "SIT");
    add_desk_icon(sit, false);
    lv_obj_align(lv_obj_get_child(sit, 0), LV_ALIGN_CENTER, 34, 0);
    bind_preset(sit, 3);

    s_drawer_toggle = theme::make_button(rail, LV_SYMBOL_RIGHT);
    lv_obj_set_size(s_drawer_toggle, 84, 64);
    lv_obj_set_ignore_layout(s_drawer_toggle, true);
    lv_obj_align(s_drawer_toggle, l.rail_right ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0,
                 0);
    lv_obj_add_event_cb(s_drawer_toggle, manual_clicked_cb, LV_EVENT_CLICKED, nullptr);
}

// The rail widens rather than a card floating over the page: these controls belong to the rail.
constexpr std::int32_t DRAWER_W  = 340;
constexpr std::uint32_t DRAWER_MS = 200;

// With the rail on the right the drawer opens towards the content, so its left
// edge travels as it widens.
void place_drawer(std::int32_t width)
{
    const Layout l = layout();
    lv_obj_set_width(s_drawer, width);
    lv_obj_set_x(s_drawer, l.rail_right ? l.screen_w - RAIL_W - width : RAIL_W);
}

void drawer_width_cb(void *, std::int32_t value) { place_drawer(value); }

void animate_drawer(bool open)
{
    s_drawer_open = open;
    // The arrow points the way the drawer will go.
    if (s_drawer_toggle != nullptr) {
        theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                        open != s_rail_right ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
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
    lv_obj_set_y(s_drawer, 0);
    lv_obj_set_height(s_drawer, l.screen_h);
    place_drawer(0);
    theme::style_panel(s_drawer, theme::panel, 0);
    // Children are clipped to the object, so at zero width the buttons are simply not drawn.
    lv_obj_set_style_pad_all(s_drawer, PANEL_PAD, 0);
    lv_obj_set_flex_flow(s_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_drawer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_drawer, BUTTON_GAP, 0);

    create_move_button(s_drawer, LV_SYMBOL_UP, Move::Up, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
    create_move_button(s_drawer, LV_SYMBOL_DOWN, Move::Down, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);

    for (int index = 0; index < 2; ++index) {
        char label[16];
        std::snprintf(label, sizeof(label), "PRESET %d", index + 1);
        lv_obj_t *btn = theme::make_button(s_drawer, label);
        lv_obj_set_size(btn, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
        theme::fill_accent(btn, LV_STATE_CHECKED);
        bind_preset(btn, index);
        register_desk_control(btn);
    }
}

void place_for_side()
{
    const Layout l = layout();
    lv_obj_set_pos(s_rail, l.rail_x, 0);
    lv_obj_set_pos(s_content, l.content_x, GAP);
    theme::align(s_drawer_toggle,
                 l.rail_right ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                    s_drawer_open != l.rail_right ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    place_strip();
    place_drawer(lv_obj_get_width(s_drawer));
    lv_obj_set_pos(s_notice_scrim, l.rail_right ? 0 : RAIL_W, 0);
    lv_obj_align(s_notice_card, LV_ALIGN_CENTER, l.content_x + l.content_w / 2 - l.screen_w / 2,
                 GAP + l.content_h / 2 - l.screen_h / 2);
}

void paint_choice(lv_obj_t *const buttons[2], bool second)
{
    for (int i = 0; i < 2; ++i) {
        const bool chosen = second == (i == 1);
        lv_obj_set_state(buttons[i], LV_STATE_CHECKED, chosen);
        theme::set_text_color(lv_obj_get_child(buttons[i], 0),
                              chosen ? theme::text : theme::secondary);
    }
}

void paint_side_buttons()
{
    paint_choice(s_side_buttons, s_rail_right);
}

void apply_rail_side(bool right)
{
    s_rail_right = right;
    place_for_side();
    paint_side_buttons();
}

// The arc carries an integer, so everything is scaled to tenths of a degree and divided back
// out; without that a 0.5 step is not representable.
constexpr int   DIAL_SCALE     = 10;
constexpr float DEFAULT_MIN_C  = 15.0f;
constexpr float DEFAULT_MAX_C  = 30.0f;
constexpr float DEFAULT_STEP_C = 0.5f;

constexpr std::int32_t DIAL_CARD_W = 480;
// Leaves a square of space in the top corners for the toggles, clear of the arc, so a tap on
// one is never read as a drag of the setpoint.
constexpr std::int32_t DIAL_INSET    = 100;
constexpr std::int32_t DIAL_CHIP     = 56;
constexpr std::int32_t DIAL_CHIP_GAP = 10;

lv_obj_t *s_dial         = nullptr;
lv_obj_t *s_dial_current = nullptr;
lv_obj_t *s_dial_target  = nullptr;
lv_obj_t *s_dial_mode    = nullptr;
lv_obj_t *s_dial_toggles[kDialToggleCount] = {};

float s_dial_step = DEFAULT_STEP_C;
// Updates are ignored while a finger is down, or the setpoint jumps back under the thumb.
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
    // On release, not on every step of the drag: one setpoint, not thirty on the way to it.
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
    // Centred in the card, not in what is left under the toggles: that left the ring visibly low.
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
    theme::arc_accent(s_dial, LV_PART_INDICATOR);
    theme::fill_accent(s_dial, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_dial, 10, LV_PART_KNOB);

    lv_obj_add_event_cb(s_dial, arc_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_dial, arc_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_dial, arc_released_cb, LV_EVENT_PRESS_LOST, nullptr);

    const std::int32_t centre = ring_y + ring / 2;
    lv_obj_align(theme::make_label(card, "CURRENT", theme::secondary, fonts::size_16()),
                 LV_ALIGN_TOP_MID, 0, centre - 80);
    s_dial_current = theme::make_label(card, "--", theme::text, fonts::temp_64());
    lv_obj_align(s_dial_current, LV_ALIGN_TOP_MID, 0, centre - 58);
    lv_obj_align(theme::make_label(card, "TARGET", theme::secondary, fonts::size_16()),
                 LV_ALIGN_TOP_MID, 0, centre + 24);
    s_dial_target = theme::make_accent_label(card, "--", fonts::temp_34());
    lv_obj_align(s_dial_target, LV_ALIGN_TOP_MID, 0, centre + 46);

    // After the arc, so these are on top of it and take their own presses.
    for (int i = 0; i < kDialToggleCount; ++i) {
        lv_obj_t *chip = theme::make_button(card, "", theme::panel, fonts::size_16());
        lv_obj_set_size(chip, DIAL_CHIP, DIAL_CHIP);
        theme::fill_accent(chip, LV_STATE_CHECKED);
        lv_obj_set_style_radius(chip, DIAL_CHIP / 2, 0);
        lv_obj_set_pos(chip, inner - DIAL_CHIP - i * (DIAL_CHIP + DIAL_CHIP_GAP), 0);
        lv_obj_add_event_cb(chip, dial_toggle_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        s_dial_toggles[i] = chip;
        lv_obj_set_hidden(chip, true);
    }

    // Sits in the ring's own gap: narrow enough that its corners stay within the 90 degrees the
    // arc does not draw, and clear of where the knob parks at either end.
    const std::int32_t mode_w = ring * 11 / 20;
    const std::int32_t mode_h = 68;
    s_dial_mode               = theme::make_button(card, "OFF", theme::panel);
    lv_obj_set_size(s_dial_mode, mode_w, mode_h);
    theme::fill_accent(s_dial_mode, LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_dial_mode, mode_h / 2, 0);
    lv_obj_align(s_dial_mode, LV_ALIGN_TOP_MID, 0, ring_y + ring - mode_h + 4);
    lv_obj_add_event_cb(s_dial_mode, mode_clicked_cb, LV_EVENT_CLICKED, nullptr);
}

// Heating takes the accent, and moving a shared style on or off invalidates,
// so the dial is repainted when the mode changes rather than on every report.
Hvac s_dial_shown = Hvac::Heating;  // what build_thermostat leaves on screen

void paint_dial(Hvac state)
{
    if (state == s_dial_shown) {
        return;
    }
    s_dial_shown             = state;
    const bool          heat = state == Hvac::Heating;
    const std::uint32_t ink  = state == Hvac::Idle ? theme::amber : theme::secondary;
    theme::arc_accent_or(s_dial, heat, ink, LV_PART_INDICATOR);
    theme::fill_accent_or(s_dial, heat, ink, LV_PART_KNOB);
}

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

// Fixed widths, shared out: sizing each pill to its text shuffled the whole row sideways
// whenever a reading gained or lost a digit.
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

        lv_obj_t *dot = lv_obj_create(pill);
        lv_obj_set_size(dot, DOT, DOT);
        theme::style_panel(dot, theme::secondary, DOT / 2);
        lv_obj_set_clickable(dot, false);

        lv_obj_t *column = lv_obj_create(pill);
        lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(column, 1);
        theme::style_panel(column, theme::panel_light, 0);
        lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
        lv_obj_set_clickable(column, false);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        // Tightens the two rows without touching the pill's size. All upper case, so nothing
        // descends far enough to clip.
        lv_obj_set_style_pad_row(column, -1, 0);

        lv_obj_t *label = theme::make_label(column, "", theme::secondary, fonts::size_16());
        lv_obj_t *value = theme::make_label(column, "", theme::text, fonts::size_22());
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);

        s_pills[i] = Pill{pill, dot, column, label, value, false};
        lv_obj_set_hidden(pill, true);
    }
}

constexpr std::int32_t BULB_W = 34;
constexpr std::int32_t BULB_H = 48;

lv_obj_t *s_lights_button = nullptr;
lv_obj_t *s_lights_name   = nullptr;
lv_obj_t *s_lights_state  = nullptr;
lv_obj_t *s_bulbs[kLightCount]   = {};
bool      s_light_on[kLightCount] = {};
bool      s_lights_on             = false;
// A long press fires LONG_PRESSED and then CLICKED on release, so without this one gesture
// would both open the picker and toggle the lights.
bool s_lights_long = false;

std::optional<ModalOverlay> s_light_picker;
struct LightButton {
    lv_obj_t *root  = nullptr;
    lv_obj_t *name  = nullptr;
    lv_obj_t *state = nullptr;
};
LightButton s_lights[kLightCount];

// Two independent things decide a bulb's colour: whether that light is on, and
// whether the button it is drawn on has lit up -- a lit bulb on a lit button
// would otherwise be the accent on the accent. Both are object states, so the
// four colours are four selectors rather than a pair worked out here.
void bulb_states(lv_obj_t *part)
{
    theme::fill_accent(part, LV_STATE_CHECKED);
    theme::fill_dim_accent(part, LV_STATE_USER_1);
    lv_obj_set_style_bg_color(part, lv_color_hex(theme::text),
                              LV_STATE_CHECKED | LV_STATE_USER_1);
}

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
    bulb_states(glass);
    lv_obj_set_clickable(glass, false);

    lv_obj_t *base = lv_obj_create(bulb);
    lv_obj_set_size(base, 16, BULB_H - BULB_W - 3);
    lv_obj_set_pos(base, (BULB_W - 16) / 2, BULB_W + 3);
    theme::style_panel(base, theme::disabled_ink, 3);
    bulb_states(base);
    lv_obj_set_clickable(base, false);
    return bulb;
}

void paint_bulbs()
{
    for (int i = 0; i < kLightCount; ++i) {
        if (s_bulbs[i] == nullptr) {
            continue;
        }
        for (int part = 0; part < 2; ++part) {
            lv_obj_t *obj = lv_obj_get_child(s_bulbs[i], part);
            lv_obj_set_state(obj, LV_STATE_CHECKED, s_light_on[i]);
            lv_obj_set_state(obj, LV_STATE_USER_1, s_lights_on);
        }
    }
}

void paint_light(lv_obj_t *root, lv_obj_t *name, lv_obj_t *state, bool on)
{
    lv_obj_set_state(root, LV_STATE_CHECKED, on);
    theme::set_text_color(name, on ? theme::text : theme::secondary);
    theme::set_text_color(state, theme::text);
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
    theme::fill_accent(s_lights_button, LV_STATE_CHECKED);
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

    lv_obj_t *title = theme::make_accent_label(card, "LIGHTS", fonts::size_22());
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
        theme::fill_accent(btn, LV_STATE_CHECKED);
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

    s_light_picker->add_close_button();
}

lv_obj_t *s_media_card   = nullptr;
lv_obj_t *s_media_frame  = nullptr;
lv_obj_t *s_media_art    = nullptr;
lv_obj_t *s_media_source = nullptr;
lv_obj_t *s_media_title  = nullptr;
lv_obj_t *s_media_artist = nullptr;
// Alternated with the two pixel buffers behind them, so a new cover never arrives under a
// source pointer LVGL has already seen.
lv_image_dsc_t s_art_dsc[2]{};
int            s_art_slot = 0;
bool           s_media_long   = false;
bool           s_media_swiped = false;

std::optional<ModalOverlay> s_media_panel;
lv_obj_t *s_panel_frame    = nullptr;
lv_obj_t *s_panel_art      = nullptr;
lv_obj_t *s_panel_title    = nullptr;
lv_obj_t *s_panel_artist   = nullptr;
lv_obj_t *s_panel_play     = nullptr;
lv_obj_t *s_panel_progress = nullptr;
lv_obj_t *s_panel_elapsed  = nullptr;
lv_obj_t *s_panel_total    = nullptr;
lv_obj_t *s_panel_quieter  = nullptr;
lv_obj_t *s_panel_louder   = nullptr;
lv_obj_t *s_panel_volume_pct  = nullptr;
lv_obj_t *s_panel_volume_icon = nullptr;
lv_timer_t *s_progress_timer = nullptr;

struct TextBox {
    std::int32_t x;
    std::int32_t w;
};
TextBox s_card_with_art{}, s_card_bare{};
TextBox s_panel_with_art{}, s_panel_bare{};
bool    s_has_art = false;

constexpr std::int32_t CARD_TITLE_Y = 28;
constexpr std::int32_t VOL_W        = 88;
constexpr std::int32_t VOL_H        = 60;

std::int32_t s_card_inner_h  = 0;
std::int32_t s_panel_inner_h = 0;
std::int32_t s_panel_below   = 0;  // what the rows under the artist need

std::int32_t artist_height(const lv_font_t *font, std::int32_t available)
{
    const std::int32_t line = lv_font_get_line_height(font);
    return available >= 2 * line ? 2 * line : line;
}

// The label keeps a fixed height so DOTS truncates rather than grows, so whatever sits under it
// has to be told where the text actually ended.
std::int32_t title_height(const char *text, const lv_font_t *font, std::int32_t width)
{
    const std::int32_t line = lv_font_get_line_height(font);
    if (text == nullptr || text[0] == '\0') {
        return line;
    }
    lv_point_t size{};
    lv_text_get_size(&size, text, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    return size.y > line ? 2 * line : line;
}

// From where the title really ends: fixed offsets left a line-sized gap under any one-line title.
void layout_media_text()
{
    const TextBox card  = s_has_art ? s_card_with_art : s_card_bare;
    const TextBox panel = s_has_art ? s_panel_with_art : s_panel_bare;

    const std::int32_t card_title =
        title_height(lv_label_get_text(s_media_title), fonts::size_20(), card.w);
    const std::int32_t card_artist_y = CARD_TITLE_Y + card_title + 6;
    const std::int32_t card_artist_h =
        artist_height(fonts::size_16(), s_card_inner_h - card_artist_y);
    lv_obj_set_height(s_media_artist, card_artist_h);
    theme::align(s_media_artist, LV_ALIGN_TOP_LEFT, card.x, card_artist_y);

    std::int32_t y = title_height(lv_label_get_text(s_panel_title), fonts::size_28(), panel.w) + 6;
    const std::int32_t panel_artist_h =
        artist_height(fonts::size_22(), s_panel_inner_h - s_panel_below - y);
    lv_obj_set_height(s_panel_artist, panel_artist_h);
    theme::align(s_panel_artist, LV_ALIGN_TOP_LEFT, panel.x, y);

    y += panel_artist_h + 20;
    theme::align(s_panel_progress, LV_ALIGN_TOP_LEFT, panel.x, y);

    y += 14;
    theme::align(s_panel_elapsed, LV_ALIGN_TOP_LEFT, panel.x, y);
    theme::align(s_panel_total, LV_ALIGN_TOP_RIGHT, 0, y);

    y += lv_font_get_line_height(fonts::size_16()) + 20;
    const std::int32_t text_dy = (VOL_H - lv_font_get_line_height(fonts::size_20())) / 2;
    theme::align(s_panel_volume_icon, LV_ALIGN_TOP_LEFT, panel.x, y + text_dy);
    theme::align(s_panel_volume_pct, LV_ALIGN_TOP_LEFT, panel.x + 34, y + text_dy);
    theme::align(s_panel_quieter, LV_ALIGN_TOP_RIGHT, -(VOL_W + 12), y);
    theme::align(s_panel_louder, LV_ALIGN_TOP_RIGHT, 0, y);
}

// The last position reported and when we heard it, so the bar keeps moving between updates.
int        s_position_s   = 0;
int        s_duration_s   = 0;
bool       s_media_playing = false;
TickType_t s_position_at  = 0;

// LV_LABEL_LONG_MODE_DOTS only truncates once the text exceeds the label's height, and a label
// left at content height simply grows -- which is how a long title ended up over the artist.
void one_line(lv_obj_t *label, const lv_font_t *font, std::int32_t width)
{
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

void two_lines(lv_obj_t *label, const lv_font_t *font, std::int32_t width)
{
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, 2 * lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

// A radius on an image widget does nothing: clipping happens on a parent, so the cover goes in
// a container that carries the radius and clips what it holds.
lv_obj_t *rounded_frame(lv_obj_t *parent, std::int32_t side, std::int32_t radius)
{
    lv_obj_t *frame = lv_obj_create(parent);
    lv_obj_set_size(frame, side, side);
    theme::style_panel(frame, theme::panel, radius);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_clickable(frame, false);
    return frame;
}

void write_clock(lv_obj_t *label, int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
    theme::set_text(label, text);
}

// Tenths of a second, not whole ones: a second is a visible step across a bar this wide.
constexpr int PROGRESS_TICK_MS = 200;
constexpr int PROGRESS_SCALE   = 10;  // bar units per second

void progress_tick(lv_timer_t *)
{
    if (!s_media_panel.has_value() || !s_media_panel->visible() || s_duration_s <= 0) {
        return;
    }
    int tenths = s_position_s * PROGRESS_SCALE;
    if (s_media_playing) {
        const TickType_t since = xTaskGetTickCount() - s_position_at;
        tenths += static_cast<int>(since * PROGRESS_SCALE / configTICK_RATE_HZ);
    }
    const int limit = s_duration_s * PROGRESS_SCALE;
    tenths          = tenths > limit ? limit : tenths;
    lv_bar_set_value(s_panel_progress, tenths, LV_ANIM_OFF);
    write_clock(s_panel_elapsed, tenths / PROGRESS_SCALE);
}

// Skipping a track takes the player through paused or idle on the way to the
// next one, and flashing the cover grey for a moment each time reads as a
// glitch. A short wait tells a real pause from a gap between tracks.
constexpr std::uint32_t PAUSE_SETTLE_MS = 1500;

lv_timer_t *s_pause_timer     = nullptr;
bool        s_playing_shown   = false;
bool        s_has_track_shown = false;

void apply_playing(bool playing)
{
    s_playing_shown = playing;
    theme::set_text(lv_obj_get_child(s_panel_play, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    // Compared first: neither setter checks, so writing this unconditionally
    // repainted both covers -- one 200 pixels square out of PSRAM -- on every
    // entity update.
    const lv_opa_t dim = s_has_track_shown && !playing ? LV_OPA_50 : LV_OPA_TRANSP;
    for (lv_obj_t *art : {s_media_art, s_panel_art}) {
        if (lv_obj_get_style_image_recolor_opa(art, LV_PART_MAIN) != dim) {
            lv_obj_set_style_image_recolor(art, lv_color_hex(theme::background), 0);
            lv_obj_set_style_image_recolor_opa(art, dim, 0);
        }
    }
}

void cancel_pause_settle()
{
    if (s_pause_timer != nullptr) {
        lv_timer_delete(s_pause_timer);
        s_pause_timer = nullptr;
    }
}

void pause_settled(lv_timer_t *)
{
    cancel_pause_settle();
    apply_playing(false);
}

void media_action_cb(lv_event_t *e)
{
    const auto action =
        static_cast<MediaAction>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    // Same as pressing the card: a deliberate pause shows at once rather than
    // waiting out the delay that exists for track changes.
    if (action == MediaAction::PlayPause) {
        cancel_pause_settle();
        apply_playing(!s_playing_shown);
    }
    if (s_handlers.media != nullptr) {
        s_handlers.media(action);
    }
}

void media_card_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    // A gesture still ends in a release over the card, so it has to be remembered and the click
    // that follows dropped, or every skip would also toggle play.
    if (code == LV_EVENT_GESTURE) {
        const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
        if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) {
            return;
        }
        s_media_swiped = true;
        if (s_handlers.media != nullptr) {
            s_handlers.media(direction == LV_DIR_LEFT ? MediaAction::Next : MediaAction::Previous);
        }
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        s_media_long = true;
        if (s_media_panel.has_value()) {
            s_media_panel->open(s_media_card);
        }
        return;
    }
    if (std::exchange(s_media_long, false) || std::exchange(s_media_swiped, false)) {
        return;
    }
    // Pressing pause here is not a gap between tracks, so it shows at once --
    // the wait is only there to ride out what a player does while skipping.
    cancel_pause_settle();
    apply_playing(!s_playing_shown);
    if (s_handlers.media != nullptr) {
        s_handlers.media(MediaAction::PlayPause);
    }
}

lv_obj_t *media_button(lv_obj_t *parent, const char *symbol, MediaAction action, std::int32_t w,
                       std::int32_t h)
{
    lv_obj_t *button = theme::make_button(parent, symbol, theme::panel_light, fonts::size_32());
    lv_obj_set_size(button, w, h);
    lv_obj_add_event_cb(button, media_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(action)));
    return button;
}

void build_media_panel(lv_obj_t *parent)
{
    constexpr std::int32_t PAD    = 24;
    constexpr std::int32_t ART    = 200;
    constexpr std::int32_t BTN_H  = 92;
    constexpr std::int32_t CARD_W = 700;
    constexpr std::int32_t CARD_H = 408;

    s_media_panel.emplace(parent, CARD_W, CARD_H);
    lv_obj_t *card = s_media_panel->content();
    lv_obj_set_style_pad_all(card, PAD, 0);

    const std::int32_t inner_w = CARD_W - 2 * PAD;
    const std::int32_t inner_h = CARD_H - 2 * PAD;
    s_panel_inner_h            = inner_h;
    // What the artist has to leave room for: the bar, the clocks, the volume row and the transport.
    s_panel_below = 20 + 8 + 14 + lv_font_get_line_height(fonts::size_16()) + 20 + VOL_H + 16 +
                    BTN_H;

    s_panel_frame = rounded_frame(card, ART, 18);
    lv_obj_align(s_panel_frame, LV_ALIGN_TOP_LEFT, 0, 0);
    s_panel_art = lv_image_create(s_panel_frame);
    lv_obj_set_size(s_panel_art, ART, ART);
    lv_obj_center(s_panel_art);
    lv_image_set_inner_align(s_panel_art, LV_IMAGE_ALIGN_STRETCH);

    s_panel_with_art = {ART + PAD, inner_w - ART - PAD};
    s_panel_bare     = {0, inner_w};
    const std::int32_t text_x = s_panel_with_art.x;
    const std::int32_t text_w = s_panel_with_art.w;

    s_panel_title = theme::make_label(card, "--", theme::text, fonts::size_28());
    two_lines(s_panel_title, fonts::size_28(), text_w);
    lv_obj_align(s_panel_title, LV_ALIGN_TOP_LEFT, text_x, 0);

    s_panel_artist = theme::make_label(card, "", theme::secondary, fonts::size_22());
    one_line(s_panel_artist, fonts::size_22(), text_w);

    s_panel_progress = lv_bar_create(card);
    lv_obj_set_size(s_panel_progress, text_w, 8);
    theme::style_panel(s_panel_progress, theme::panel_light, 4);
    theme::fill_accent(s_panel_progress, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_panel_progress, 4, LV_PART_INDICATOR);

    s_panel_elapsed = theme::make_label(card, "0:00", theme::secondary, fonts::size_16());
    s_panel_total   = theme::make_label(card, "0:00", theme::secondary, fonts::size_16());

    s_panel_volume_icon =
        theme::make_label(card, LV_SYMBOL_VOLUME_MAX, theme::secondary, fonts::size_20());
    s_panel_volume_pct = theme::make_label(card, "--", theme::text, fonts::size_20());

    s_panel_quieter = media_button(card, LV_SYMBOL_MINUS, MediaAction::VolumeDown, VOL_W, VOL_H);
    s_panel_louder  = media_button(card, LV_SYMBOL_PLUS, MediaAction::VolumeUp, VOL_W, VOL_H);

    constexpr std::int32_t SIDE_W = 150;
    constexpr std::int32_t PLAY_W = 200;
    constexpr std::int32_t GAP    = 16;
    const std::int32_t     row_w  = 2 * SIDE_W + PLAY_W + 2 * GAP;
    const std::int32_t     row_x  = (inner_w - row_w) / 2;
    const std::int32_t     row_y  = inner_h - BTN_H;

    lv_obj_t *previous = media_button(card, LV_SYMBOL_PREV, MediaAction::Previous, SIDE_W, BTN_H);
    lv_obj_align(previous, LV_ALIGN_TOP_LEFT, row_x, row_y);

    s_panel_play = media_button(card, LV_SYMBOL_PLAY, MediaAction::PlayPause, PLAY_W, BTN_H);
    lv_obj_align(s_panel_play, LV_ALIGN_TOP_LEFT, row_x + SIDE_W + GAP, row_y);
    theme::fill_accent(s_panel_play);

    lv_obj_t *next = media_button(card, LV_SYMBOL_NEXT, MediaAction::Next, SIDE_W, BTN_H);
    lv_obj_align(next, LV_ALIGN_TOP_LEFT, row_x + SIDE_W + PLAY_W + 2 * GAP, row_y);

    s_progress_timer = lv_timer_create(progress_tick, PROGRESS_TICK_MS, nullptr);

    s_media_panel->add_close_button();
}

void build_media_card(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                      std::int32_t h)
{
    s_media_card = lv_button_create(parent);
    lv_obj_set_pos(s_media_card, x, y);
    lv_obj_set_size(s_media_card, w, h);
    theme::style_button(s_media_card, theme::panel_light);
    lv_obj_set_style_pad_all(s_media_card, 18, 0);
    lv_obj_add_event_cb(s_media_card, media_card_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_media_card, media_card_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(s_media_card, media_card_cb, LV_EVENT_GESTURE, nullptr);
    // Gestures bubble by default, so LVGL walks past the card to the screen and a handler here
    // never runs. Claim them.
    lv_obj_remove_flag(s_media_card, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // Smaller than the card allows: a square filling the height left a third of the width for
    // the title.
    const std::int32_t art = h - 61;
    s_media_frame          = rounded_frame(s_media_card, art, 14);
    lv_obj_align(s_media_frame, LV_ALIGN_LEFT_MID, 0, 0);
    s_media_art = lv_image_create(s_media_frame);
    lv_obj_set_size(s_media_art, art, art);
    lv_obj_center(s_media_art);
    lv_image_set_inner_align(s_media_art, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_hidden(s_media_frame, true);

    s_card_inner_h  = h - 36;
    s_card_with_art = {art + 18, w - 36 - art - 18};
    s_card_bare     = {0, w - 36};
    const std::int32_t text_x = s_card_bare.x;
    const std::int32_t text_w = s_card_bare.w;

    s_media_source = theme::make_label(s_media_card, "SPEAKER", theme::secondary, fonts::size_16());
    lv_obj_align(s_media_source, LV_ALIGN_TOP_LEFT, text_x, 0);

    s_media_title = theme::make_label(s_media_card, "--", theme::text, fonts::size_20());
    two_lines(s_media_title, fonts::size_20(), text_w);
    lv_obj_align(s_media_title, LV_ALIGN_TOP_LEFT, text_x, 28);

    s_media_artist = theme::make_label(s_media_card, "", theme::secondary, fonts::size_16());
    one_line(s_media_artist, fonts::size_16(), text_w);
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

    // Letting this column wrap across the page is what put the tiles on top of the thermostat.
    const std::int32_t col_x    = DIAL_CARD_W + BUTTON_GAP;
    const std::int32_t col_w    = inner_w - col_x;
    const std::int32_t lights_h = body_h * 5 / 9;
    build_lights_button(page, col_x, body_y, col_w, lights_h);

    const std::int32_t media_y = body_y + lights_h + BUTTON_GAP;
    build_media_card(page, col_x, media_y, col_w, inner_h - media_y);

    // Last, so the overlays cover the page rather than being covered by it.
    build_light_picker(page);
    build_media_panel(page);
}

constexpr int PAGE_COUNT = 5;
lv_obj_t *s_pages[PAGE_COUNT]    = {};
lv_obj_t *s_nav_tabs[PAGE_COUNT] = {};

struct NavItem {
    const char *icon;
    const char *caption;
    // Hidden while the tracked phone is away. Home stays because the desk has to work for
    // anyone, and Setup stays because it is where the presence readout explains the rest.
    bool        needs_presence;
};
constexpr NavItem NAV_ITEMS[PAGE_COUNT] = {
    {LV_SYMBOL_HOME, "Home", false},    {LV_SYMBOL_LIST, "Stats", true},
    {LV_SYMBOL_BELL, "Alerts", true},   {LV_SYMBOL_GPS, "Radar", true},
    {LV_SYMBOL_SETTINGS, "Setup", false},
};

// Nothing is gated until the phone has been recognised once: hiding half the panel because a
// key is wrong, or because Bluetooth never came up, is worse than not gating at all.
bool s_presence_known = false;
bool s_present        = false;
int  s_page           = 0;

constexpr int RADAR_PAGE = 3;
constexpr int SETUP_PAGE = 4;
// Read from the diagnostics task, which does no work at all while the page it
// fills is not the one on screen.
std::atomic<bool> s_setup_visible{false};

// Off by setting, and ignored until the phone has been recognised once.
bool s_presence_gate = true;

bool page_available(int index)
{
    return !NAV_ITEMS[index].needs_presence || !s_presence_gate || !s_presence_known || s_present;
}

void select_page(int index)
{
    if (!page_available(index)) {
        index = 0;
    }
    s_page = index;
    s_setup_visible.store(index == SETUP_PAGE, std::memory_order_relaxed);
    if (index == SETUP_PAGE && s_handlers.diagnostics != nullptr) {
        s_handlers.diagnostics();
    }
    if (index == RADAR_PAGE) {
        radar_page_opened();
    }
    if (s_handlers.radar != nullptr) {
        s_handlers.radar(index == RADAR_PAGE, page_available(RADAR_PAGE));
    }
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_set_hidden(s_nav_tabs[i], !page_available(i));
        lv_obj_set_hidden(s_pages[i], i != index);
        const bool active = (i == index);
        lv_obj_set_state(s_nav_tabs[i], LV_STATE_CHECKED, active);
        const std::uint32_t ink = active ? theme::text : theme::secondary;
        theme::set_text_color(lv_obj_get_child(s_nav_tabs[i], 0), ink);
        theme::set_text_color(lv_obj_get_child(s_nav_tabs[i], 1), ink);
    }
}

void nav_event_cb(lv_event_t *e)
{
    select_page(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
}

lv_obj_t *s_brightness_value = nullptr;

void brightness_event_cb(lv_event_t *e)
{
    auto      *slider  = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int  percent = static_cast<int>(lv_slider_get_value(slider));

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(s_brightness_value, text);

    if (s_handlers.brightness != nullptr) {
        s_handlers.brightness(percent);
    }
}

void build_placeholder_page(lv_obj_t *page, const char *title, const char *blurb)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 14, 0);
    theme::make_label(page, title, theme::text, fonts::size_32());
    theme::make_label(page, blurb, theme::secondary, fonts::size_20());
}

constexpr int INFO_COUNT = static_cast<int>(Info::Count);
lv_obj_t     *s_info[INFO_COUNT] = {};

struct InfoRow {
    Info        field;
    const char *label;
};

// summary is the row the tile shows, so a subsystem says something useful
// before it is opened. setting is the one control that belongs to it, if any.
struct InfoCard {
    const char *title;
    const char *icon;
    const InfoRow *rows;
    int            count;
    Info           summary;
    bool           has_setting;
    Setting        setting;
    const char    *setting_label;
};

constexpr InfoRow NETWORK_ROWS[] = {
    {Info::WifiState, "Wi-Fi"},   {Info::WifiSsid, "Network"},  {Info::WifiIp, "Address"},
    {Info::WifiMac, "MAC"},       {Info::WifiSignal, "Signal"}, {Info::WifiChannel, "Channel"},
};
constexpr InfoRow HASS_ROWS[] = {
    {Info::HaBroker, "Broker"},
    {Info::HaSocket, "Socket"},
    {Info::HaEntities, "Entities"},
};
constexpr InfoRow BLUETOOTH_ROWS[] = {
    {Info::PhoneRadio, "Radio"},
    {Info::BleLink, "Desk proxy"},
    {Info::BleTrip, "Round trip"},
    {Info::BleLoss, "Dropped"},
};
constexpr InfoRow PRESENCE_ROWS[] = {
    {Info::PhoneKey, "Identity key"},
    {Info::PhoneState, "Phone"},
    {Info::PhoneSignal, "Signal"},
};
constexpr InfoRow POWER_ROWS[] = {
    {Info::PowerSource, "Source"},   {Info::PowerCharge, "Charge"},
    {Info::PowerVolts, "Voltage"},   {Info::PowerCurrent, "Current"},
    {Info::PowerStatus, "State"},
};
constexpr InfoRow DESK_ROWS[] = {
    {Info::DeskTransport, "Driven over"}, {Info::DeskLink, "Controller"},
    {Info::DeskHeight, "Height"},         {Info::DeskActive, "Standing at"},
    {Info::DeskStand, "Stand"},           {Info::DeskSit, "Sit"},
    {Info::DeskOne, "Preset 1"},          {Info::DeskTwo, "Preset 2"},
};
constexpr InfoRow SYSTEM_ROWS[] = {
    {Info::SysFirmware, "Firmware"}, {Info::SysBuilt, "Built"},  {Info::SysUptime, "Uptime"},
    {Info::SysRam, "Internal free"}, {Info::SysPsram, "PSRAM free"},
    {Info::SysRamLow, "Low mark"},
};

constexpr InfoCard INFO_CARDS[] = {
    {"Network", LV_SYMBOL_WIFI, NETWORK_ROWS, static_cast<int>(std::size(NETWORK_ROWS)),
     Info::WifiState, false, Setting::Charging, nullptr},
    {"Home Assistant", LV_SYMBOL_HOME, HASS_ROWS, static_cast<int>(std::size(HASS_ROWS)),
     Info::HaSocket, false, Setting::Charging, nullptr},
    {"Bluetooth", LV_SYMBOL_BLUETOOTH, BLUETOOTH_ROWS,
     static_cast<int>(std::size(BLUETOOTH_ROWS)), Info::BleLink, false, Setting::Charging,
     nullptr},
    {"Presence", LV_SYMBOL_EYE_OPEN, PRESENCE_ROWS, static_cast<int>(std::size(PRESENCE_ROWS)),
     Info::PhoneState, true, Setting::PresenceGate, "Hide pages while away"},
    {"Power", LV_SYMBOL_BATTERY_FULL, POWER_ROWS, static_cast<int>(std::size(POWER_ROWS)),
     Info::PowerCharge, true, Setting::Charging, "Charge the battery"},
    {"Desk", LV_SYMBOL_UP, DESK_ROWS, static_cast<int>(std::size(DESK_ROWS)), Info::DeskLink,
     true, Setting::DeskBluetooth, "Drive it over Bluetooth"},
    {"System", LV_SYMBOL_SETTINGS, SYSTEM_ROWS, static_cast<int>(std::size(SYSTEM_ROWS)),
     Info::SysUptime, false, Setting::Charging, nullptr},
};
constexpr int INFO_CARD_COUNT = static_cast<int>(std::size(INFO_CARDS));
static_assert(INFO_CARD_COUNT == static_cast<int>(Subsystem::Count), "a tile per subsystem");

constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);

constexpr std::int32_t ROW_CARD_H   = 88;
constexpr std::int32_t DIAG_HEADER_H = 56;
// The log card is one size whatever it holds; the state card is only as tall
// as the subsystem it is showing.
constexpr std::int32_t LOG_W        = 800;
constexpr std::int32_t LOG_H        = 520;
constexpr std::int32_t DETAIL_W     = 660;
constexpr std::int32_t DETAIL_ROW_GAP = 2;
constexpr std::int32_t DETAIL_SET_TOP = 14;
constexpr std::int32_t DETAIL_SET_H   = 56;
// Clear air under the title and the close button before the content starts.
constexpr std::int32_t HEADER_GAP   = 22;
// Past this the card would crowd the page, so it scrolls instead. Everything
// the panel reports fits well inside it.
constexpr std::int32_t DETAIL_MAX_FRAC = 80;
constexpr std::int32_t DETAIL_PAD   = 24;
constexpr std::int32_t DETAIL_ROW_H = 30;
constexpr std::int32_t TILE_DOT     = 14;
constexpr std::size_t  LOG_TEXT_MAX = 4096;

lv_obj_t *s_settings_view   = nullptr;
lv_obj_t *s_appearance_view = nullptr;
lv_obj_t *s_diag_view       = nullptr;
lv_obj_t *s_diag_summary    = nullptr;
lv_obj_t *s_volume_value    = nullptr;
lv_obj_t *s_volume_slider   = nullptr;

lv_obj_t *s_tile_value[INFO_CARD_COUNT] = {};
lv_obj_t *s_tile_dot[INFO_CARD_COUNT]   = {};
lv_obj_t *s_detail[INFO_CARD_COUNT]     = {};
Level     s_card_level[INFO_CARD_COUNT] = {};
lv_obj_t *s_detail_title                = nullptr;
int       s_detail_shown                = -1;
std::int32_t s_detail_height[INFO_CARD_COUNT] = {};
// A long press is followed by a click on release, which would drop the state
// card straight on top of the log that was just opened.
bool s_tile_long = false;

lv_obj_t *s_setting_value[SETTING_COUNT] = {};
bool      s_setting_on[SETTING_COUNT]    = {};

// Which tile mirrors this row, or -1. Saves the summary needing its own setter.
int s_summary_card[INFO_COUNT] = {};

std::optional<ModalOverlay> s_diagnostics;
std::optional<ModalOverlay> s_log_modal;
lv_obj_t                   *s_log_title = nullptr;
lv_obj_t                   *s_log_text  = nullptr;
int                         s_log_shown = -1;
lv_timer_t                 *s_log_timer = nullptr;

std::uint32_t info_ink(Level level)
{
    switch (level) {
        case Level::Good: return theme::green;
        case Level::Warn: return theme::amber;
        case Level::Bad:  return theme::red;
        default:          return theme::text;
    }
}

void refresh_diag_summary()
{
    int poor = 0;
    for (Level level : s_card_level) {
        if (level == Level::Warn || level == Level::Bad) {
            ++poor;
        }
    }
    char text[48];
    if (poor == 0) {
        std::snprintf(text, sizeof(text), "%d subsystems, all healthy", INFO_CARD_COUNT);
    } else {
        std::snprintf(text, sizeof(text), "%d of %d need attention", poor, INFO_CARD_COUNT);
    }
    theme::set_text(s_diag_summary, text);
    theme::set_text_color(s_diag_summary, poor == 0 ? theme::secondary : theme::amber);
}

void refresh_log()
{
    if (s_log_shown < 0 || s_handlers.log == nullptr) {
        return;
    }
    // Four kilobytes held for the whole uptime to be used only while a log is
    // on screen, and held in the one internal pool DMA can reach. Taken from
    // PSRAM when it is actually wanted instead.
    auto *text = static_cast<char *>(
        heap_caps_malloc(LOG_TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (text == nullptr) {
        return;
    }
    text[0] = '\0';
    s_handlers.log(INFO_CARDS[s_log_shown].title, text, LOG_TEXT_MAX);
    theme::set_text(s_log_text, text[0] != '\0' ? text : "Nothing logged yet");
    heap_caps_free(text);
}

// Deliberately does not scroll: new lines arrive under whatever is being read,
// and yanking the view back to the bottom every second made it unreadable.
void log_tick(lv_timer_t *)
{
    if (s_log_modal.has_value() && s_log_modal->visible()) {
        refresh_log();
    }
}

void detail_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (!s_diagnostics.has_value() || std::exchange(s_tile_long, false)) {
        return;
    }
    for (int i = 0; i < INFO_CARD_COUNT; ++i) {
        lv_obj_set_hidden(s_detail[i], i != index);
    }
    s_detail_shown = index;
    theme::set_text(s_detail_title, INFO_CARDS[index].title);
    lv_obj_scroll_to_y(s_detail[index], 0, LV_ANIM_OFF);
    s_diagnostics->resize(DETAIL_W, s_detail_height[index]);
    s_diagnostics->open();
}

void log_held_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (!s_log_modal.has_value()) {
        return;
    }
    s_tile_long = true;
    s_log_shown = index;
    char title[48];
    std::snprintf(title, sizeof(title), "%s log", INFO_CARDS[index].title);
    theme::set_text(s_log_title, title);
    refresh_log();
    lv_obj_scroll_to_y(lv_obj_get_parent(s_log_text), 0, LV_ANIM_OFF);
    s_log_modal->open();
}

void show_diagnostics_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_settings_view, true);
    lv_obj_set_hidden(s_diag_view, false);
}

void show_appearance_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_settings_view, true);
    lv_obj_set_hidden(s_appearance_view, false);
}

void show_settings_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_diag_view, true);
    lv_obj_set_hidden(s_appearance_view, true);
    lv_obj_set_hidden(s_settings_view, false);
}

void apply_setting(int index, bool on)
{
    s_setting_on[index] = on;
    if (s_setting_value[index] != nullptr) {
        theme::set_text(s_setting_value[index], on ? "On" : "Off");
        theme::ink_accent_or(s_setting_value[index], on, theme::secondary);
    }
    if (static_cast<Setting>(index) == Setting::PresenceGate) {
        s_presence_gate = on;
        select_page(s_page);
    }
}

void setting_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    const bool next = !s_setting_on[index];
    apply_setting(index, next);
    if (s_handlers.setting != nullptr) {
        s_handlers.setting(static_cast<Setting>(index), next);
    }
}

void restart_held_cb(lv_event_t *)
{
    if (s_handlers.restart != nullptr) {
        s_handlers.restart();
    }
}

void volume_changed_cb(lv_event_t *e)
{
    auto     *slider  = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int percent = static_cast<int>(lv_slider_get_value(slider));

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(s_volume_value, text);

    if (s_handlers.volume != nullptr) {
        s_handlers.volume(percent, lv_event_get_code(e) == LV_EVENT_RELEASED);
    }
}

lv_obj_t *build_tile(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h, const char *icon, const char *title)
{
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    theme::style_button(tile, theme::panel_light);
    lv_obj_set_style_pad_all(tile, 20, 0);

    lv_obj_t *glyph = theme::make_accent_label(tile, icon, fonts::size_28());
    lv_obj_align(glyph, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *caption = theme::make_label(tile, title, theme::secondary, fonts::size_22());
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 44, 4);
    lv_obj_set_width(caption, w - 96);
    lv_obj_set_height(caption, fonts::size_22()->line_height);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS);
    return tile;
}

lv_obj_t *tile_value(lv_obj_t *tile, std::int32_t w, const char *initial)
{
    lv_obj_t *value = theme::make_label(tile, initial, theme::text, fonts::size_32());
    lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_width(value, w - 40);
    lv_obj_set_height(value, fonts::size_32()->line_height);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    return value;
}

lv_obj_t *build_info_row(lv_obj_t *parent, const char *label, const lv_font_t *font,
                         std::int32_t height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    theme::style_panel(row, theme::panel, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    theme::make_label(row, label, theme::secondary, font);

    lv_obj_t *value = theme::make_label(row, "--", theme::text, font);
    lv_obj_set_flex_grow(value, 1);
    // DOTS needs a bounded box in both directions, so the height is pinned to
    // one line as well as the width coming from the grow.
    lv_obj_set_height(value, font->line_height);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    return value;
}

void build_info_tile(lv_obj_t *parent, int index, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h)
{
    lv_obj_t *tile =
        build_tile(parent, x, y, w, h, INFO_CARDS[index].icon, INFO_CARDS[index].title);
    lv_obj_add_event_cb(tile, detail_clicked_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_add_event_cb(tile, log_held_cb, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, TILE_DOT, TILE_DOT);
    theme::style_panel(dot, theme::secondary, TILE_DOT / 2);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_clickable(dot, false);

    s_tile_value[index] = tile_value(tile, w, "--");
    s_tile_dot[index]   = dot;
    s_summary_card[static_cast<int>(INFO_CARDS[index].summary)] = index;
}

std::int32_t detail_height(const InfoCard &card)
{
    // Every gap the flex column puts between the rows counts, and leaving them
    // out is what left every card a few pixels short and therefore scrolling.
    std::int32_t body = card.count * DETAIL_ROW_H + (card.count - 1) * DETAIL_ROW_GAP;
    if (card.has_setting) {
        body += DETAIL_ROW_GAP + DETAIL_SET_TOP + DETAIL_SET_H;
    }

    const Layout       l   = layout();
    const std::int32_t cap = std::min(l.screen_h * DETAIL_MAX_FRAC / 100, l.content_h - 2 * GAP);
    return std::min(2 * DETAIL_PAD + ModalOverlay::header_height() + HEADER_GAP + body, cap);
}

void build_detail_overlay(lv_obj_t *parent)
{
    s_diagnostics.emplace(parent, DETAIL_W, detail_height(INFO_CARDS[0]));
    lv_obj_t *card = s_diagnostics->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    s_detail_title = theme::make_accent_label(card, "", fonts::size_28());
    lv_obj_align(s_detail_title, LV_ALIGN_TOP_LEFT, 0, 6);

    const std::int32_t width  = DETAIL_W - 2 * DETAIL_PAD;
    const std::int32_t body_y = ModalOverlay::header_height() + HEADER_GAP;

    for (int i = 0; i < INFO_CARD_COUNT; ++i) {
        s_detail_height[i]        = detail_height(INFO_CARDS[i]);
        const std::int32_t height = s_detail_height[i] - 2 * DETAIL_PAD - body_y;
        lv_obj_t *panel = lv_obj_create(card);
        lv_obj_set_pos(panel, 0, body_y);
        lv_obj_set_size(panel, width, height);
        theme::style_panel(panel, theme::panel, 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
        lv_obj_set_hidden(panel, true);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(panel, DETAIL_ROW_GAP, 0);
        lv_obj_set_scrollable(panel, true);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);

        for (int r = 0; r < INFO_CARDS[i].count; ++r) {
            const InfoRow &row = INFO_CARDS[i].rows[r];
            s_info[static_cast<int>(row.field)] =
                build_info_row(panel, row.label, fonts::size_20(), DETAIL_ROW_H);
        }

        if (INFO_CARDS[i].has_setting) {
            const int index  = static_cast<int>(INFO_CARDS[i].setting);
            lv_obj_t *button = lv_button_create(panel);
            lv_obj_set_size(button, LV_PCT(100), DETAIL_SET_H);
            lv_obj_set_style_margin_top(button, DETAIL_SET_TOP, 0);
            theme::style_button(button, theme::panel_light);
            lv_obj_set_style_pad_hor(button, 16, 0);
            lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(button, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_add_event_cb(button, setting_clicked_cb, LV_EVENT_CLICKED,
                                reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

            theme::make_label(button, INFO_CARDS[i].setting_label, theme::text, fonts::size_20());
            s_setting_value[index] =
                theme::make_label(button, "Off", theme::secondary, fonts::size_20());
        }

        s_detail[i] = panel;
    }

    s_diagnostics->add_close_button();
}

// Its own card, because a log wants the whole surface and its own dark ground
// rather than a strip under a table.
void build_log_overlay(lv_obj_t *parent)
{
    s_log_modal.emplace(parent, LOG_W, LOG_H);
    lv_obj_t *card = s_log_modal->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    s_log_title = theme::make_accent_label(card, "", fonts::size_28());
    lv_obj_align(s_log_title, LV_ALIGN_TOP_LEFT, 0, 6);

    const std::int32_t width  = LOG_W - 2 * DETAIL_PAD;
    const std::int32_t body_y = ModalOverlay::header_height() + HEADER_GAP;
    const std::int32_t height = LOG_H - 2 * DETAIL_PAD - body_y;

    lv_obj_t *pane = lv_obj_create(card);
    lv_obj_set_pos(pane, 0, body_y);
    lv_obj_set_size(pane, width, height);
    theme::style_panel(pane, theme::background, 12);
    lv_obj_set_style_pad_all(pane, 14, 0);
    lv_obj_set_scrollable(pane, true);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);

    s_log_text = theme::make_label(pane, "", theme::secondary, fonts::size_16());
    lv_obj_set_width(s_log_text, width - 28);
    lv_label_set_long_mode(s_log_text, LV_LABEL_LONG_MODE_WRAP);

    s_log_modal->add_close_button();
    s_log_timer = lv_timer_create(log_tick, 1000, nullptr);
}

lv_obj_t *build_slider_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                            const char *title, int value, int low, lv_event_cb_t changed,
                            lv_obj_t **out_value)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_pos(root, 0, y);
    lv_obj_set_size(root, w, ROW_CARD_H);
    theme::style_panel(root, theme::panel_light, 14);
    lv_obj_set_style_pad_hor(root, 22, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(root, 18, 0);

    theme::make_accent_label(root, icon, fonts::size_28());

    lv_obj_t *caption = theme::make_label(root, title, theme::secondary, fonts::size_22());
    lv_obj_set_width(caption, 210);

    lv_obj_t *slider = lv_slider_create(root);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 18);
    // The bar is the whole hit area otherwise, and a finger rarely lands inside
    // eighteen pixels of it.
    lv_obj_set_ext_click_area(slider, 26);
    lv_slider_set_range(slider, low, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme::panel), LV_PART_MAIN);
    theme::fill_accent(slider, LV_PART_INDICATOR);
    theme::fill_accent(slider, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, changed, LV_EVENT_RELEASED, nullptr);

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", value);
    *out_value = theme::make_label(root, text, theme::text, fonts::size_22());
    lv_obj_set_width(*out_value, 64);
    lv_obj_set_style_text_align(*out_value, LV_TEXT_ALIGN_RIGHT, 0);
    return slider;
}

constexpr std::int32_t SWATCH      = 128;
constexpr int          SWATCH_COLS = 5;
constexpr std::int32_t ACCENT_DOT  = 44;
constexpr std::int32_t SIDE_BTN_W  = 150;
constexpr std::int32_t SIDE_BTN_H  = 60;

std::optional<ModalOverlay> s_colour_picker;
lv_obj_t                   *s_swatch_tick[theme::primaries.size()] = {};

void apply_primary(std::uint32_t colour)
{
    theme::set_primary(colour);
    for (std::size_t i = 0; i < theme::primaries.size(); ++i) {
        lv_obj_set_hidden(s_swatch_tick[i], theme::primaries[i] != colour);
    }
}

void swatch_clicked_cb(lv_event_t *e)
{
    const auto index =
        static_cast<std::size_t>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    apply_primary(theme::primaries[index]);
    if (s_colour_picker.has_value()) {
        s_colour_picker->close();
    }
    if (s_handlers.primary != nullptr) {
        s_handlers.primary(theme::primaries[index]);
    }
}

void accent_card_cb(lv_event_t *)
{
    if (s_colour_picker.has_value()) {
        s_colour_picker->open();
    }
}

void side_clicked_cb(lv_event_t *e)
{
    const bool right = lv_event_get_user_data(e) != nullptr;
    if (right == s_rail_right) {
        return;
    }
    apply_rail_side(right);
    if (s_handlers.rail_side != nullptr) {
        s_handlers.rail_side(right);
    }
}

void flip_clicked_cb(lv_event_t *e)
{
    const bool flipped = lv_event_get_user_data(e) != nullptr;
    if (flipped == s_flipped) {
        return;
    }
    s_flipped = flipped;
    board::set_flipped(flipped);
    paint_choice(s_flip_buttons, flipped);
    if (s_handlers.orientation != nullptr) {
        s_handlers.orientation(flipped);
    }
}

// The same shell as a slider card: icon, caption, then whatever the setting is
// operated with. A handler makes it a button, so the whole row is the target.
lv_obj_t *build_row_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                         const char *title, lv_event_cb_t clicked)
{
    lv_obj_t *root = clicked != nullptr ? lv_button_create(parent) : lv_obj_create(parent);
    lv_obj_set_pos(root, 0, y);
    lv_obj_set_size(root, w, ROW_CARD_H);
    if (clicked != nullptr) {
        theme::style_button(root, theme::panel_light);
        lv_obj_add_event_cb(root, clicked, LV_EVENT_CLICKED, nullptr);
    } else {
        theme::style_panel(root, theme::panel_light, 14);
    }
    lv_obj_set_style_pad_hor(root, 22, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(root, 18, 0);

    theme::make_accent_label(root, icon, fonts::size_28());
    lv_obj_set_flex_grow(theme::make_label(root, title, theme::secondary, fonts::size_22()), 1);
    return root;
}

void build_accent_card(lv_obj_t *parent, std::int32_t y, std::int32_t w)
{
    lv_obj_t *root = build_row_card(parent, y, w, LV_SYMBOL_TINT, "Accent colour", accent_card_cb);

    lv_obj_t *dot = lv_obj_create(root);
    lv_obj_set_size(dot, ACCENT_DOT, ACCENT_DOT);
    theme::style_panel(dot, theme::panel, ACCENT_DOT / 2);
    theme::fill_accent(dot);
    lv_obj_set_clickable(dot, false);

    theme::make_label(root, LV_SYMBOL_RIGHT, theme::secondary, fonts::size_28());
}

// The pair carries the choice at LV_STATE_CHECKED, so showing it again is a
// state change rather than a restyle, and the accent follows on its own.
void build_choice_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                       const char *title, const char *first, const char *second,
                       lv_event_cb_t clicked, lv_obj_t *out[2])
{
    lv_obj_t         *root    = build_row_card(parent, y, w, icon, title, nullptr);
    const char *const text[2] = {first, second};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *btn = theme::make_button(root, text[i], theme::panel, fonts::size_22());
        lv_obj_set_size(btn, SIDE_BTN_W, SIDE_BTN_H);
        theme::fill_accent(btn, LV_STATE_CHECKED);
        lv_obj_add_event_cb(btn, clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        out[i] = btn;
    }
}

// Swatches, not sliders: a hue is chosen by eye, and the target has to be found
// by a fingertip on a wall panel.
void build_colour_picker(lv_obj_t *parent)
{
    const int rows = (static_cast<int>(theme::primaries.size()) + SWATCH_COLS - 1) / SWATCH_COLS;
    const std::int32_t body_y = ModalOverlay::header_height() + HEADER_GAP;
    const std::int32_t card_w =
        SWATCH_COLS * SWATCH + (SWATCH_COLS - 1) * BUTTON_GAP + 2 * DETAIL_PAD;
    const std::int32_t card_h = body_y + rows * SWATCH + (rows - 1) * BUTTON_GAP + 2 * DETAIL_PAD;

    s_colour_picker.emplace(parent, card_w, card_h);
    lv_obj_t *card = s_colour_picker->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    lv_obj_t *title = theme::make_accent_label(card, "Accent colour", fonts::size_28());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 6);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_pos(grid, 0, body_y);
    lv_obj_set_size(grid, card_w - 2 * DETAIL_PAD, card_h - 2 * DETAIL_PAD - body_y);
    theme::style_panel(grid, theme::panel, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, BUTTON_GAP, 0);
    lv_obj_set_style_pad_column(grid, BUTTON_GAP, 0);

    for (std::size_t i = 0; i < theme::primaries.size(); ++i) {
        const std::uint32_t colour = theme::primaries[i];
        lv_obj_t           *btn    = lv_button_create(grid);
        lv_obj_set_size(btn, SWATCH, SWATCH);
        theme::style_button(btn, colour);
        lv_obj_set_style_radius(btn, 18, 0);
        // Its own colour darkened rather than the accent's: pressing a swatch
        // must not flash the colour it is about to replace.
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::dim_of(colour)), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, swatch_clicked_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));

        lv_obj_t *tick = theme::make_label(btn, LV_SYMBOL_OK, theme::background, fonts::size_32());
        lv_obj_center(tick);
        lv_obj_set_hidden(tick, colour != theme::primary);
        s_swatch_tick[i] = tick;
    }

    s_colour_picker->add_close_button();
}

lv_obj_t *build_sub_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_set_pos(view, 0, 0);
    lv_obj_set_size(view, w, h);
    theme::style_panel(view, theme::panel, 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    return view;
}

void build_sub_header(lv_obj_t *view, const char *title)
{
    lv_obj_t *back = lv_button_create(view);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_size(back, 120, DIAG_HEADER_H);
    theme::style_button(back, theme::panel_light);
    lv_obj_center(theme::make_label(back, LV_SYMBOL_LEFT, theme::text, fonts::size_28()));
    lv_obj_add_event_cb(back, show_settings_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = theme::make_label(view, title, theme::text, fonts::size_28());
    lv_obj_set_pos(label, 140, 12);
}

lv_obj_t *build_page_tile(lv_obj_t *parent, std::int32_t y, std::int32_t w, std::int32_t h,
                          const char *icon, const char *title, lv_event_cb_t clicked)
{
    lv_obj_t *tile = build_tile(parent, 0, y, w, h, icon, title);
    lv_obj_add_event_cb(tile, clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *chevron = theme::make_label(tile, LV_SYMBOL_RIGHT, theme::secondary,
                                          fonts::size_28());
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    return tile;
}

void build_settings_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);

    s_volume_slider = build_slider_card(view, 0, w, LV_SYMBOL_VOLUME_MAX, "Notification volume", 0,
                                        0, volume_changed_cb, &s_volume_value);

    const std::int32_t tiles_y = ROW_CARD_H + BUTTON_GAP;
    const std::int32_t tile_h  = (h - tiles_y - 2 * BUTTON_GAP) / 3;
    const std::int32_t pitch   = tile_h + BUTTON_GAP;

    lv_obj_t *look = build_page_tile(view, tiles_y, w, tile_h, LV_SYMBOL_IMAGE, "Appearance",
                                     show_appearance_cb);
    theme::set_text_color(tile_value(look, w, "Colour, sidebar, orientation, brightness"),
                          theme::secondary);

    lv_obj_t *diag = build_page_tile(view, tiles_y + pitch, w, tile_h, LV_SYMBOL_LIST,
                                     "Diagnostics", show_diagnostics_cb);
    s_diag_summary = tile_value(diag, w, "");
    theme::set_text_color(s_diag_summary, theme::secondary);

    lv_obj_t *restart = build_tile(view, 0, tiles_y + 2 * pitch, w, tile_h, LV_SYMBOL_POWER,
                                   "Restart panel");
    lv_obj_add_event_cb(restart, restart_held_cb, LV_EVENT_LONG_PRESSED, nullptr);
    theme::set_text_color(tile_value(restart, w, "Hold to restart"), theme::secondary);

    s_settings_view = view;
}

void build_appearance_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);
    lv_obj_set_hidden(view, true);
    build_sub_header(view, "Appearance");

    const std::int32_t body_y = DIAG_HEADER_H + BUTTON_GAP;
    // Stacked with an ordinary gap rather than spread to fill the page: four
    // rows pushed to the corners of all that room read as four unrelated
    // things rather than one list.
    const std::int32_t pitch = ROW_CARD_H + BUTTON_GAP;

    build_slider_card(view, body_y, w, LV_SYMBOL_EYE_OPEN, "Brightness", s_initial_brightness,
                      board::kMinBrightness, brightness_event_cb, &s_brightness_value);
    build_accent_card(view, body_y + pitch, w);
    build_choice_card(view, body_y + 2 * pitch, w, LV_SYMBOL_BARS, "Sidebar", "LEFT", "RIGHT",
                      side_clicked_cb, s_side_buttons);
    paint_side_buttons();
    build_choice_card(view, body_y + 3 * pitch, w, LV_SYMBOL_REFRESH, "Orientation", "NORMAL",
                      "FLIPPED", flip_clicked_cb, s_flip_buttons);
    paint_choice(s_flip_buttons, s_flipped);

    s_appearance_view = view;
}

void build_diagnostics_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);
    lv_obj_set_hidden(view, true);
    build_sub_header(view, "Diagnostics");

    const std::int32_t grid_y = DIAG_HEADER_H + BUTTON_GAP;
    const int          rows   = (INFO_CARD_COUNT + 2) / 3;
    const std::int32_t tile_w = (w - 2 * BUTTON_GAP) / 3;
    const std::int32_t tile_h = (h - grid_y - (rows - 1) * BUTTON_GAP) / rows;

    for (int i = 0; i < INFO_CARD_COUNT; ++i) {
        build_info_tile(view, i, (i % 3) * (tile_w + BUTTON_GAP),
                        grid_y + (i / 3) * (tile_h + BUTTON_GAP), tile_w, tile_h);
    }

    s_diag_view = view;
}

void build_settings_page(lv_obj_t *page)
{
    const Layout l = layout();

    lv_obj_set_style_pad_all(page, PANEL_PAD, 0);
    const std::int32_t inner_w = l.content_w - 2 * PANEL_PAD;
    const std::int32_t inner_h = l.content_h - 2 * PANEL_PAD;

    for (int &card : s_summary_card) {
        card = -1;
    }

    build_settings_view(page, inner_w, inner_h);
    build_appearance_view(page, inner_w, inner_h);
    build_diagnostics_view(page, inner_w, inner_h);
    build_detail_overlay(page);
    build_log_overlay(page);
    build_colour_picker(page);
    refresh_diag_summary();
}

void create_content(lv_obj_t *parent)
{


    const Layout l = layout();

    lv_obj_t *area = lv_obj_create(parent);
    s_content      = area;
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
        theme::fill_accent(tab, LV_STATE_CHECKED);
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
    {
        const Layout rl = layout();
        lv_obj_set_style_pad_all(s_pages[3], PANEL_PAD, 0);
        build_radar_page(s_pages[3], rl.content_w - 2 * PANEL_PAD, rl.content_h - 2 * PANEL_PAD);
    }
    build_settings_page(s_pages[4]);
    select_page(0);
}

lv_obj_t   *s_splash       = nullptr;
lv_obj_t   *s_splash_bar   = nullptr;
lv_obj_t   *s_splash_step  = nullptr;
lv_obj_t   *s_splash_top   = nullptr;
lv_obj_t   *s_splash_leg[2] = {};
lv_timer_t *s_splash_guard = nullptr;
bool        s_splash_up    = false;
lv_timer_t  *s_splash_tick  = nullptr;
std::int32_t s_splash_shown  = 0;
std::int32_t s_splash_floor  = 0;
std::uint32_t s_splash_start = 0;
bool         s_splash_ready  = false;

// Hundredths of a percent: at whole percent each step moved the bar almost
// four pixels, which reads as ticking however often it runs.
constexpr std::int32_t SPLASH_FULL = 10000;

constexpr std::int32_t DESK_W     = 260;
constexpr std::int32_t DESK_H     = 150;
constexpr std::int32_t DESK_BAR   = 14;
constexpr std::int32_t DESK_LOW   = DESK_H - 58;
constexpr std::int32_t DESK_HIGH  = 6;

void splash_hide(lv_anim_t *)
{
    lv_obj_set_hidden(s_splash, true);
    if (s_splash_tick != nullptr) {
        lv_timer_delete(s_splash_tick);
        s_splash_tick = nullptr;
    }
}

void splash_opa(void *target, std::int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(target), static_cast<lv_opa_t>(value), 0);
}

// The desk stands up as the panel comes up.
void splash_raise(std::int32_t progress)
{
    lv_bar_set_value(s_splash_bar, progress, LV_ANIM_OFF);

    const std::int32_t top = DESK_LOW - (DESK_LOW - DESK_HIGH) * progress / SPLASH_FULL;
    lv_obj_set_y(s_splash_top, top);
    for (lv_obj_t *leg : s_splash_leg) {
        lv_obj_set_y(leg, top + DESK_BAR);
        lv_obj_set_height(leg, DESK_H - DESK_BAR - 10 - top);
    }
}

// Measured on the device: the display wakes at 3.9 s and Home Assistant is
// authenticated at 12.3 s.
constexpr std::uint32_t SPLASH_EXPECTED_MS = 8400;
constexpr std::int32_t  SPLASH_PREDICTED   = 90 * SPLASH_FULL / 100;
constexpr std::int32_t  SPLASH_TAIL        = 99 * SPLASH_FULL / 100;

// Driven by the clock rather than by the startup steps. The steps are seconds
// apart and unevenly spaced, so following them meant standing still through
// the long waits however smoothly each jump was eased. Since how long the
// whole thing takes is known, the bar runs to its own schedule and eases out
// as it approaches it; if the panel is not ready by then it keeps going,
// slower and slower, rather than stopping. A step that lands ahead of
// schedule pulls it forward.
void splash_animate(lv_timer_t *)
{
    const std::uint32_t elapsed = lv_tick_elaps(s_splash_start);

    std::int32_t target;
    if (s_splash_ready) {
        target = SPLASH_FULL;
    } else if (elapsed < SPLASH_EXPECTED_MS) {
        const float t = static_cast<float>(elapsed) / SPLASH_EXPECTED_MS;
        target = static_cast<std::int32_t>(SPLASH_PREDICTED * (1.0f - (1.0f - t) * (1.0f - t)));
    } else {
        const float over = static_cast<float>(elapsed - SPLASH_EXPECTED_MS) / 4000.0f;
        target = SPLASH_PREDICTED +
                 static_cast<std::int32_t>((SPLASH_TAIL - SPLASH_PREDICTED) *
                                           (1.0f - std::exp(-over)));
    }
    target = target > s_splash_floor ? target : s_splash_floor;

    if (s_splash_shown == target) {
        return;
    }
    const std::int32_t gap  = target - s_splash_shown;
    const std::int32_t step = gap / 12;
    s_splash_shown += step != 0 ? step : (gap > 0 ? 1 : -1);
    splash_raise(s_splash_shown);
}

void splash_expired(lv_timer_t *)
{
    s_splash_guard = nullptr;
    ESP_ERROR_CHECK_WITHOUT_ABORT(splash_done());
}

lv_obj_t *splash_bar(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h, std::uint32_t colour)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    theme::style_panel(bar, colour, 3);
    lv_obj_set_clickable(bar, false);
    return bar;
}

void build_splash()
{
    const Layout l = layout();

    s_splash = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_size(s_splash, l.screen_w, l.screen_h);
    theme::style_panel(s_splash, theme::background, 0);
    lv_obj_set_clickable(s_splash, true);
    s_splash_up = true;

    lv_obj_t *desk = lv_obj_create(s_splash);
    lv_obj_set_size(desk, DESK_W, DESK_H);
    lv_obj_align(desk, LV_ALIGN_CENTER, 0, -110);
    theme::style_panel(desk, theme::background, 0);
    lv_obj_set_style_bg_opa(desk, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(desk, false);

    splash_bar(desk, 6, DESK_H - 10, 76, 10, theme::panel_light);
    splash_bar(desk, DESK_W - 82, DESK_H - 10, 76, 10, theme::panel_light);
    s_splash_leg[0] = splash_bar(desk, 34, 0, DESK_BAR, 10, theme::panel_light);
    s_splash_leg[1] = splash_bar(desk, DESK_W - 34 - DESK_BAR, 0, DESK_BAR, 10,
                                 theme::panel_light);
    s_splash_top = splash_bar(desk, 0, 0, DESK_W, DESK_BAR, theme::panel_light);
    theme::fill_accent(s_splash_top);

    lv_obj_align(theme::make_label(s_splash, "SMART FLEXISPOT", theme::text, fonts::size_48()),
                 LV_ALIGN_CENTER, 0, 40);
    lv_obj_align(theme::make_label(s_splash, "Wouter ten Brinke", theme::secondary,
                                   fonts::size_20()),
                 LV_ALIGN_CENTER, 0, 86);

    s_splash_bar = lv_bar_create(s_splash);
    lv_obj_set_size(s_splash_bar, 360, 6);
    lv_obj_align(s_splash_bar, LV_ALIGN_CENTER, 0, 138);
    theme::style_panel(s_splash_bar, theme::panel_light, 3);
    theme::fill_accent(s_splash_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_splash_bar, 3, LV_PART_INDICATOR);
    lv_bar_set_range(s_splash_bar, 0, SPLASH_FULL);

    s_splash_step = theme::make_label(s_splash, "starting", theme::secondary, fonts::size_16());
    lv_obj_align(s_splash_step, LV_ALIGN_CENTER, 0, 164);

    splash_raise(0);
    s_splash_start = lv_tick_get();
    s_splash_tick  = lv_timer_create(splash_animate, 16, nullptr);

    // Home Assistant is reachable about eight seconds after the display wakes,
    // so this is the backstop, not the usual path: telemetry dismisses it as
    // soon as the network, the broker and the socket are all up.
    s_splash_guard = lv_timer_create(splash_expired, 10000, nullptr);
    lv_timer_set_repeat_count(s_splash_guard, 1);
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
    build_splash();  // last, so it covers everything until startup finishes
}

}  // namespace

esp_err_t splash_step(const char *label, int percent)
{
    ESP_RETURN_ON_FALSE(s_splash != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (!s_splash_up) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    theme::set_text(s_splash_step, label);
    const std::int32_t wanted = percent * SPLASH_FULL / 100;
    if (wanted > s_splash_floor) {
        s_splash_floor = wanted;
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t splash_done()
{
    ESP_RETURN_ON_FALSE(s_splash != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    if (s_splash_up) {
        s_splash_up = false;
        s_splash_ready = true;
        theme::set_text(s_splash_step, "ready");

        // A full-screen blend per frame. Affordable once at boot with nothing
        // else running; not a pattern to reuse for anything on top of a page.
        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, s_splash);
        lv_anim_set_exec_cb(&fade, splash_opa);
        lv_anim_set_values(&fade, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_delay(&fade, 500);
        lv_anim_set_duration(&fade, 380);
        lv_anim_set_completed_cb(&fade, splash_hide);
        lv_anim_start(&fade);

        if (s_splash_guard != nullptr) {
            lv_timer_delete(s_splash_guard);
            s_splash_guard = nullptr;
        }
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t init(const Handlers &handlers, int initial_brightness, std::uint32_t accent,
               bool rail_right, bool flipped)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    // Before any label exists: the fonts carry the fallback Montserrat lacks.
    fonts::init();
    // Both before anything is built, so nothing is ever drawn in last week's
    // colour or on the wrong side and then moved.
    theme::init_accents();
    if (accent != 0) {
        theme::set_primary(accent);
    }
    s_rail_right         = rail_right;
    s_flipped            = flipped;
    s_handlers           = handlers;
    s_initial_brightness = initial_brightness;
    build_screen();
    // Draw before returning: the caller lights the backlight next, and what is
    // on the panel until the first flush is whatever it powered up holding.
    lv_refr_now(nullptr);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_preset_active(int index, bool active)
{
    ESP_RETURN_ON_FALSE(index >= 0 && index < kPresetCount, ESP_ERR_INVALID_ARG, TAG, "preset %d",
                        index);
    ESP_RETURN_ON_FALSE(s_preset_buttons[index] != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    if (s_preset_active[index] == active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    s_preset_active[index] = active;
    lv_obj_t *button       = s_preset_buttons[index];
    lv_obj_set_state(button, LV_STATE_CHECKED, active);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_t *child = lv_obj_get_child(button, i);
        theme::set_text_color(child, theme::text);
        for (std::uint32_t j = 0; j < lv_obj_get_child_count(child); ++j) {
            lv_obj_set_state(lv_obj_get_child(child, j), LV_STATE_CHECKED, active);
        }
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_height(int height_mm)
{
    ESP_RETURN_ON_FALSE(s_height.has_value(), ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    s_height->set_tenths(height_mm);
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
        // Labels inherit the state's text colour only if they were not given one of their own.
        lv_obj_set_state(obj, LV_STATE_DISABLED, !available);
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        if (label != nullptr) {
            theme::set_text_color(label, available ? theme::text : theme::disabled_ink);
        }
        // A press that cannot reach the desk should do nothing rather than appear to work.
        lv_obj_set_clickable(obj, available);
    }
    // The readout is left alone: blanking it from two places is how it ended up stuck empty.
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_media(const char *source, const char *title, const char *artist, const char *state,
                    bool playing)
{
    ESP_RETURN_ON_FALSE(s_media_card != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    const bool has_track = title != nullptr && title[0] != '\0';
    theme::set_text(s_media_source, source != nullptr && source[0] != '\0' ? source : "SPEAKER");
    theme::set_text(s_media_title, has_track ? title : (state != nullptr ? state : "--"));
    theme::set_text(s_media_artist, has_track && artist != nullptr ? artist : "");
    theme::set_text_color(s_media_title, has_track ? theme::text : theme::secondary);

    theme::set_text(s_panel_title, has_track ? title : (state != nullptr ? state : "--"));
    theme::set_text(s_panel_artist, has_track && artist != nullptr ? artist : "");
    layout_media_text();

    s_has_track_shown = has_track;
    if (playing || !has_track) {
        cancel_pause_settle();
        apply_playing(playing);
    } else if (!s_playing_shown) {
        apply_playing(false);
    } else if (s_pause_timer == nullptr) {
        s_pause_timer = lv_timer_create(pause_settled, PAUSE_SETTLE_MS, nullptr);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_media_progress(int position_s, int duration_s, bool playing)
{
    ESP_RETURN_ON_FALSE(s_panel_progress != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    s_position_s    = position_s;
    s_duration_s    = duration_s;
    s_media_playing = playing;
    s_position_at   = xTaskGetTickCount();

    const bool known = duration_s > 0;
    lv_obj_set_hidden(s_panel_progress, !known);
    lv_obj_set_hidden(s_panel_elapsed, !known);
    lv_obj_set_hidden(s_panel_total, !known);
    if (known) {
        lv_bar_set_range(s_panel_progress, 0, duration_s * PROGRESS_SCALE);
        lv_bar_set_value(s_panel_progress, position_s * PROGRESS_SCALE, LV_ANIM_OFF);
        write_clock(s_panel_elapsed, position_s);
        write_clock(s_panel_total, duration_s);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_media_volume(int percent)
{
    ESP_RETURN_ON_FALSE(s_panel_volume_pct != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(s_panel_volume_pct, text);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_album_art(const void *pixels, bool placeholder)
{
    ESP_RETURN_ON_FALSE(s_media_art != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    const bool has_art = pixels != nullptr;
    const bool framed  = has_art || placeholder;
    lv_obj_set_hidden(s_media_frame, !framed);
    lv_obj_set_hidden(s_panel_frame, !framed);
    lv_obj_set_hidden(s_media_art, !has_art);
    lv_obj_set_hidden(s_panel_art, !has_art);

    const TextBox card  = framed ? s_card_with_art : s_card_bare;
    const TextBox panel = framed ? s_panel_with_art : s_panel_bare;
    theme::align(s_media_source, LV_ALIGN_TOP_LEFT, card.x, 0);
    theme::align(s_media_title, LV_ALIGN_TOP_LEFT, card.x, 28);
    lv_obj_set_width(s_media_title, card.w);
    lv_obj_set_width(s_media_artist, card.w);

    theme::align(s_panel_title, LV_ALIGN_TOP_LEFT, panel.x, 0);
    s_has_art = framed;
    lv_obj_set_width(s_panel_title, panel.w);
    lv_obj_set_width(s_panel_artist, panel.w);
    lv_obj_set_width(s_panel_progress, panel.w);
    layout_media_text();

    if (!has_art) {
        lvgl_port_unlock();
        return ESP_OK;
    }

    lv_image_dsc_t &dsc = s_art_dsc[s_art_slot];
    s_art_slot          = 1 - s_art_slot;

    dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc.header.w      = media::kArtSize;
    dsc.header.h      = media::kArtSize;
    dsc.header.stride = media::kArtSize * 2;
    dsc.data_size     = media::kArtSize * media::kArtSize * 2;
    dsc.data          = static_cast<const std::uint8_t *>(pixels);

    lv_image_set_src(s_media_art, &dsc);
    lv_image_set_src(s_panel_art, &dsc);
    lv_obj_invalidate(s_media_art);
    lv_obj_invalidate(s_panel_art);
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
        lv_obj_set_state(chip, LV_STATE_CHECKED, on);
        theme::set_text_color(text, on ? theme::text : theme::secondary);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_thermostat_range(float min_c, float max_c, float step_c)
{
    ESP_RETURN_ON_FALSE(s_dial != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
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

    paint_dial(state);

    lv_obj_set_state(s_dial_mode, LV_STATE_CHECKED, state != Hvac::Off);
    lv_obj_t *mode_text = lv_obj_get_child(s_dial_mode, 0);
    theme::set_text(mode_text, mode != nullptr ? mode : "--");
    theme::set_text_color(mode_text, theme::text);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_presence(bool has_key, bool present, bool ever_seen)
{
    ESP_RETURN_ON_FALSE(s_phone_slash != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    const bool here = has_key && present;
    lv_obj_set_hidden(s_phone_slash, here);

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
    ESP_RETURN_ON_FALSE(s_clock_hours != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    char hours[4] = "--";
    char minutes[4] = "--";
    const char *colon = text != nullptr ? std::strchr(text, ':') : nullptr;
    s_clock_known = colon != nullptr;
    if (s_clock_known) {
        const std::size_t count = static_cast<std::size_t>(colon - text);
        std::snprintf(hours, sizeof(hours), "%.*s", static_cast<int>(count), text);
        std::snprintf(minutes, sizeof(minutes), "%s", colon + 1);
    } else {
        lv_obj_set_style_opa(s_clock_colon, LV_OPA_COVER, 0);
    }
    theme::set_text(s_clock_hours, hours);
    theme::set_text(s_clock_minutes, minutes);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_links(bool wifi, bool mqtt)
{
    (void)mqtt;  // the broker has its own indicator in Home Assistant
    ESP_RETURN_ON_FALSE(s_wifi_icon != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    // Called on a timer, so restyling anyway would invalidate the icon every couple of seconds.
    static int last = -1;
    if (last == (wifi ? 1 : 0)) {
        return ESP_OK;
    }
    last = wifi ? 1 : 0;
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_obj_set_hidden(s_wifi_slash, wifi);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_battery(bool present, int percent, bool charging)
{
    // Nothing on screen shows battery any more; kept so callers need not care.
    (void)present;
    (void)percent;
    (void)charging;
    return ESP_OK;
}

esp_err_t set_info(Info field, const char *value, Level level)
{
    const int index = static_cast<int>(field);
    ESP_RETURN_ON_FALSE(index >= 0 && index < INFO_COUNT, ESP_ERR_INVALID_ARG, TAG, "info %d",
                        index);
    ESP_RETURN_ON_FALSE(s_info[index] != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    const char *text = value != nullptr && value[0] != '\0' ? value : "--";
    theme::set_text(s_info[index], text);
    theme::set_text_color(s_info[index], info_ink(level));

    // The tile shows the value but takes its colour from the subsystem's health,
    // not from this row: a phone correctly reported as away is not a fault.
    const int card = s_summary_card[index];
    if (card >= 0) {
        theme::set_text(s_tile_value[card], text);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_radar(const radar::Snapshot &snapshot)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    show_radar(snapshot);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_radar_details(const char *hex, const radar::Details &details)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    show_radar_details(hex, details);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_radar_photo(const char *hex, const void *pixels, int width, int height)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    show_radar_photo(hex, pixels, width, height);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_health(Subsystem which, Level level)
{
    const int card = static_cast<int>(which);
    ESP_RETURN_ON_FALSE(card >= 0 && card < INFO_CARD_COUNT, ESP_ERR_INVALID_ARG, TAG,
                        "subsystem %d", card);
    ESP_RETURN_ON_FALSE(s_tile_dot[card] != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    theme::set_bg_color(s_tile_dot[card], level_ink(level));
    theme::set_text_color(s_tile_value[card], info_ink(level));
    if (s_card_level[card] != level) {
        s_card_level[card] = level;
        refresh_diag_summary();
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_notification_volume(int percent)
{
    ESP_RETURN_ON_FALSE(s_volume_slider != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    lv_slider_set_value(s_volume_slider, percent, LV_ANIM_OFF);
    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(s_volume_value, text);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_setting(Setting setting, bool on)
{
    const int index = static_cast<int>(setting);
    ESP_RETURN_ON_FALSE(index >= 0 && index < SETTING_COUNT, ESP_ERR_INVALID_ARG, TAG, "setting %d",
                        index);
    ESP_RETURN_ON_FALSE(s_setting_value[index] != nullptr, ESP_ERR_INVALID_STATE, TAG,
                        "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    apply_setting(index, on);
    lvgl_port_unlock();
    return ESP_OK;
}

bool diagnostics_open()
{
    return s_setup_visible.load(std::memory_order_relaxed);
}

esp_err_t notify(const char *title, const char *message, const char *level, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(s_notice_card != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    if (s_notice_count == NOTIFY_QUEUE_LEN) {
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
