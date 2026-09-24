#include "ui.h"

#include "board.h"
#include "media.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "modal_overlay.h"
#include "calendar_page.h"
#include "screenshot.h"

#ifndef SHOT_DRAWER
#define SHOT_DRAWER 0
#endif
#ifndef SHOT_ENABLED
#define SHOT_ENABLED 0
#endif
#include "radar_page.h"
#include "segment_display.h"
#include "icons.h"
#include "theme.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>

namespace ui {
namespace {
constexpr char TAG[] = "ui";

constexpr std::uint32_t LOCK_TIMEOUT_MS = 500;

// Sized from the real display rather than with percentages: LV_PCT() returns an
// encoded sentinel, so LV_PCT(100) - something lays out as nonsense.
constexpr std::int32_t RAIL_W      = 330;
// The rail is a panel like the content beside it, set in by the same gap on its
// outer sides rather than running flush to the edge of the glass.
constexpr std::int32_t RAIL_CARD_W = RAIL_W - 16;
constexpr std::int32_t NAV_H       = 92;
constexpr std::int32_t RAIL_BTN_H  = 124;
constexpr std::int32_t GAP        = 16;
constexpr std::int32_t EDGE_GAP   = 0;   // navigation sits on the bottom edge
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

bool s_rail_right = false;

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

constexpr int DESK_CONTROL_MAX = 8;
bool          s_desk_available                  = true;

lv_obj_t     *s_desk_controls[DESK_CONTROL_MAX] = {};
int           s_desk_control_count              = 0;

bool      s_screen_on         = true;

bool s_notice_lit_screen = false;

void set_screen_state(bool on)
{
    if (on == s_screen_on || s_handlers.screen == nullptr) {
        return;
    }
    s_screen_on = on;
    s_handlers.screen(on);
}

void wake_on_touch(lv_event_t *)
{
    s_notice_lit_screen = false;
    if (s_screen_on) {
        return;
    }

    // The tap that lights the screen does nothing else: whatever was under the
    // finger was not visible when it landed, and one of those things moves a desk.
    // lv_indev_active(), not lv_event_get_indev() -- the latter returns the event's
    // parameter, which is null for an event sent to the device itself, so both
    // calls below quietly did nothing. stop_processing withholds the press; the
    // reset drops the device's hold so the release cannot arrive as a click.
    lv_indev_t *indev = lv_indev_active();
    lv_indev_stop_processing(indev);
    lv_indev_reset(indev, nullptr);
    set_screen_state(true);
}

void screen_off_cb(lv_event_t *)
{
    s_notice_lit_screen = false;
    set_screen_state(false);
}

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
lv_obj_t *s_phone_icon    = nullptr;
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
        if (s_notice_lit_screen) {
            s_notice_lit_screen = false;
            set_screen_state(false);
        }
        return;
    }
    if (!s_screen_on) {
        s_notice_lit_screen = true;
        set_screen_state(true);
    }
    const Notice notice = s_notice_queue[0];
    for (int i = 1; i < s_notice_count; ++i) {
        s_notice_queue[i - 1] = s_notice_queue[i];
    }
    --s_notice_count;

    const NoticeInk ink = notice_ink(notice.level);
    theme::fill_accent_or(s_notice_bar, ink.accent, ink.colour);
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

void create_move_button(lv_obj_t *parent, const char *symbol, Move direction, std::int32_t w,
                        std::int32_t h)
{
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
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::text), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::text), LV_STATE_PRESSED);
    lv_obj_set_clickable(bar, false);
    return bar;
}

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

void place_strip()
{
    const bool right = s_rail_right;
    theme::align(s_clock_box, right ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    theme::align(s_wifi_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    theme::align(s_phone_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, right ? 44 : -44,
                 0);
}

lv_obj_t *make_rail_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = theme::make_button(parent, text);
    lv_obj_set_size(btn, RAIL_CARD_W - 2 * PANEL_PAD, RAIL_BTN_H);
    theme::fill_accent(btn, LV_STATE_CHECKED);
    register_desk_control(btn);
    return btn;
}

void preset_clicked_cb(lv_event_t *e);

// What the screen calls each preset. Everything off the screen says Preset 1 to 6.
constexpr const char *PRESET_NAMES[kPresetCount] = {
    "Preset 1", "Preset 2", "Stand", "Sit", "Sit 2", "Stand 2",
};

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
    lv_obj_set_pos(rail, l.rail_right ? l.rail_x : GAP, GAP);
    lv_obj_set_size(rail, RAIL_CARD_W, l.screen_h - 2 * GAP);
    theme::style_panel(rail, theme::panel, theme::radius::card);
    lv_obj_set_style_pad_all(rail, PANEL_PAD, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rail, BUTTON_GAP, 0);

    lv_obj_t *strip = lv_obj_create(rail);
    lv_obj_set_size(strip, RAIL_CARD_W - 2 * PANEL_PAD, 44);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_scrollable(strip, false);

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
    lv_obj_set_style_pad_left(s_clock_colon, 4, 0);
    lv_obj_set_style_pad_right(s_clock_colon, 4, 0);
    s_clock_minutes = theme::make_label(clock, "--", theme::text, fonts::size_28());
    lv_timer_create(clock_blink, 1000, nullptr);

    auto status_icon = [&](const lv_image_dsc_t *src) {
        lv_obj_t *icon = lv_image_create(strip);
        lv_image_set_src(icon, src);
        lv_obj_set_style_image_recolor(icon, lv_color_hex(theme::text), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_clickable(icon, false);
        return icon;
    };
    s_wifi_icon  = status_icon(&icons::wifi_off_icon);
    s_phone_icon = status_icon(&icons::phone_off_icon);
    place_strip();

    lv_obj_t *heading = theme::make_label(rail, "DESK HEIGHT", theme::secondary,
                                          &lv_font_montserrat_18);
    lv_obj_set_style_margin_top(heading, 18, 0);

    lv_obj_t *readout = lv_obj_create(rail);
    lv_obj_set_size(readout, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    theme::style_panel(readout, theme::panel, 0);
    lv_obj_set_style_bg_opa(readout, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(readout, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(readout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_bottom(readout, 18, 0);

    s_height.emplace(readout);
    s_height->set_tenths(-1);

    theme::make_label(readout, "CM", theme::secondary, fonts::size_22());

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

constexpr std::int32_t DRAWER_W  = 340;
constexpr std::uint32_t DRAWER_MS = 200;

// A card of its own beside the rail: tucked under it, it showed through the
// rail's rounded corners.
void place_drawer(std::int32_t width)
{
    const Layout l = layout();
    lv_obj_set_width(s_drawer, width);
    lv_obj_set_x(s_drawer, l.rail_right ? l.screen_w - RAIL_W - GAP - width : RAIL_W + GAP);
    lv_obj_set_hidden(s_drawer, width <= 0);
}

void pad_drawer() { lv_obj_set_style_pad_all(s_drawer, PANEL_PAD, 0); }

void drawer_width_cb(void *, std::int32_t value) { place_drawer(value); }

void animate_drawer(bool open)
{
    s_drawer_open = open;
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

void show_guest_presets();

void create_drawer(lv_obj_t *parent)
{
    const Layout l = layout();

    s_drawer = lv_obj_create(parent);
    lv_obj_set_y(s_drawer, GAP);
    lv_obj_set_height(s_drawer, l.screen_h - 2 * GAP);
    place_drawer(0);
    theme::style_panel(s_drawer, theme::panel, theme::radius::card);
    // It lies over the same surface, so a ring of background makes the card gap.
    lv_obj_set_style_outline_width(s_drawer, GAP, 0);
    lv_obj_set_style_outline_color(s_drawer, lv_color_hex(theme::background), 0);
    lv_obj_set_style_outline_opa(s_drawer, LV_OPA_COVER, 0);
    pad_drawer();
    lv_obj_set_flex_flow(s_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_drawer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_drawer, BUTTON_GAP, 0);

    create_move_button(s_drawer, LV_SYMBOL_UP, Move::Up, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
    create_move_button(s_drawer, LV_SYMBOL_DOWN, Move::Down, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);

    // 3 and 4 are Stand and Sit on the rail.
    for (const int index : {0, 1, 4, 5}) {
        char label[24];
        std::snprintf(label, sizeof(label), "%s", preset_name(index));
        for (char *c = label; *c != '\0'; ++c) {
            *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        }
        lv_obj_t *btn = theme::make_button(s_drawer, label);
        lv_obj_set_size(btn, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
        theme::fill_accent(btn, LV_STATE_CHECKED);
        bind_preset(btn, index);
        register_desk_control(btn);
    }
    show_guest_presets();
}

void place_for_side()
{
    const Layout l = layout();
    lv_obj_set_pos(s_rail, l.rail_right ? l.rail_x : GAP, GAP);
    lv_obj_set_pos(s_content, l.content_x, GAP);
    pad_drawer();
    place_drawer(s_drawer_open ? DRAWER_W : 0);
    theme::align(s_drawer_toggle,
                 l.rail_right ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                    s_drawer_open != l.rail_right ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    place_strip();
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

constexpr int   DIAL_SCALE     = 10;
constexpr float DEFAULT_MIN_C  = 15.0f;
constexpr float DEFAULT_MAX_C  = 30.0f;
constexpr float DEFAULT_STEP_C = 0.5f;

constexpr std::int32_t DIAL_CARD_W = 480;
constexpr std::int32_t DIAL_INSET    = 100;
constexpr std::int32_t DIAL_CHIP     = theme::chip::size;
constexpr std::int32_t DIAL_CHIP_GAP = 10;

lv_obj_t *s_dial         = nullptr;
lv_obj_t *s_dial_current = nullptr;
lv_obj_t *s_dial_target  = nullptr;
lv_obj_t *s_dial_mode    = nullptr;
lv_obj_t *s_dial_toggles[kDialToggleCount] = {};

float s_dial_step = DEFAULT_STEP_C;
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
    theme::style_panel(card, theme::panel_light, theme::radius::card);
    lv_obj_set_style_pad_all(card, PANEL_PAD, 0);

    const std::int32_t inner   = w - 2 * PANEL_PAD;
    const std::int32_t inner_h = h - 2 * PANEL_PAD;

    // Screwed down in every corner the chips leave free, like the radar's scope.
    lv_obj_set_pos(theme::make_screw(card, DIAL_CHIP), 0, 0);
    lv_obj_set_pos(theme::make_screw(card, DIAL_CHIP), 0, inner_h - DIAL_CHIP);
    lv_obj_set_pos(theme::make_screw(card, DIAL_CHIP), inner - DIAL_CHIP, inner_h - DIAL_CHIP);
    const std::int32_t ring   = std::min(w - DIAL_INSET, inner_h);
    const std::int32_t ring_y = (inner_h - ring) / 2;

    s_dial = lv_arc_create(card);
    lv_obj_set_size(s_dial, ring, ring);
    lv_obj_align(s_dial, LV_ALIGN_TOP_MID, 0, ring_y);
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

    for (int i = 0; i < kDialToggleCount; ++i) {
        lv_obj_t *chip = theme::make_chip(card, "");
        theme::fill_accent(chip, LV_STATE_CHECKED);
        lv_obj_set_pos(chip, inner - DIAL_CHIP - i * (DIAL_CHIP + DIAL_CHIP_GAP), 0);
        lv_obj_add_event_cb(chip, dial_toggle_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        s_dial_toggles[i] = chip;
        lv_obj_set_hidden(chip, true);
    }

    const std::int32_t mode_w = ring * 11 / 20;
    const std::int32_t mode_h = 68;
    s_dial_mode               = theme::make_button(card, "OFF", theme::panel);
    lv_obj_set_size(s_dial_mode, mode_w, mode_h);
    theme::fill_accent(s_dial_mode, LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_dial_mode, mode_h / 2, 0);
    lv_obj_align(s_dial_mode, LV_ALIGN_TOP_MID, 0, ring_y + ring - mode_h + 4);
    lv_obj_add_event_cb(s_dial_mode, mode_clicked_cb, LV_EVENT_CLICKED, nullptr);
}

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
// A long press fires LONG_PRESSED and then CLICKED on release, so without this
// one gesture would both open the picker and toggle the lights.
bool s_lights_long = false;

std::optional<ModalOverlay> s_light_picker;
struct LightButton {
    lv_obj_t *root  = nullptr;
    lv_obj_t *name  = nullptr;
    lv_obj_t *state = nullptr;
};
LightButton s_lights[kLightCount];

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
int       s_media_hold   = -1;  // a preset, or -1 for the media panel
bool      s_media_off    = true;  // nothing to control, so holding does nothing
lv_obj_t *s_media_frame  = nullptr;
lv_obj_t *s_media_art    = nullptr;
lv_obj_t *s_media_source = nullptr;
lv_obj_t *s_media_title  = nullptr;
lv_obj_t *s_media_artist = nullptr;
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

int        s_position_s   = 0;
int        s_duration_s   = 0;
bool       s_media_playing = false;
TickType_t s_position_at  = 0;

// LV_LABEL_LONG_MODE_DOTS only truncates once the text exceeds the label's
// height, and a label left at content height simply grows.
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

// A radius on an image widget does nothing: clipping happens on a parent, so the
// cover goes in a container that carries the radius.
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

constexpr std::uint32_t PAUSE_SETTLE_MS = 1500;

lv_timer_t *s_pause_timer     = nullptr;
bool        s_playing_shown   = false;
bool        s_has_track_shown = false;

void apply_playing(bool playing)
{
    s_playing_shown = playing;
    theme::set_text(lv_obj_get_child(s_panel_play, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

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
        if (s_media_off) {
            return;
        }
        if (s_media_hold >= 0) {
            if (s_handlers.preset != nullptr) {
                s_handlers.preset(s_media_hold, false);
            }
        } else if (s_media_panel.has_value()) {
            s_media_panel->open(s_media_card);
        }
        return;
    }
    if (std::exchange(s_media_long, false) || std::exchange(s_media_swiped, false)) {
        return;
    }
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
    // Gestures bubble by default, so LVGL walks past the card to the screen and a
    // handler here never runs.
    lv_obj_remove_flag(s_media_card, LV_OBJ_FLAG_GESTURE_BUBBLE);

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

    const std::int32_t col_x    = DIAL_CARD_W + BUTTON_GAP;
    const std::int32_t col_w    = inner_w - col_x;
    const std::int32_t lights_h = body_h * 5 / 9;
    build_lights_button(page, col_x, body_y, col_w, lights_h);

    const std::int32_t media_y = body_y + lights_h + BUTTON_GAP;
    build_media_card(page, col_x, media_y, col_w, inner_h - media_y);

    build_light_picker(page);
    build_media_panel(page);
}

constexpr int PAGE_COUNT = 5;
lv_obj_t *s_pages[PAGE_COUNT]    = {};
lv_obj_t *s_nav_tabs[PAGE_COUNT] = {};

struct NavItem {
    const char           *icon;
    const char           *caption;
    bool                  needs_presence;
    const lv_image_dsc_t *image = nullptr;  // drawn here, where the fonts have no glyph
};
constexpr NavItem NAV_ITEMS[PAGE_COUNT] = {
    {LV_SYMBOL_HOME, "Home", false},    {"", "Calendar", true, &icons::calendar_icon},
    {LV_SYMBOL_BELL, "Alerts", true},   {"", "Radar", true, &icons::plane_icon},
    {LV_SYMBOL_SETTINGS, "Setup", false},
};

bool s_presence_known = false;
bool s_present        = false;
int  s_page           = 0;

constexpr int CALENDAR_PAGE = 1;
constexpr int RADAR_PAGE = 3;
constexpr int SETUP_PAGE = 4;
std::atomic<bool> s_setup_visible{false};

bool s_presence_gate = true;

// Presets 5 and 6 are for whoever uses the desk while the phone is away.
void show_guest_presets()
{
    if (s_drawer == nullptr || s_preset_buttons[4] == nullptr) {
        return;
    }
    const bool shown = !s_presence_gate || !s_present;
    lv_obj_set_hidden(s_preset_buttons[4], !shown);
    lv_obj_set_hidden(s_preset_buttons[5], !shown);
    const Layout       l      = layout();
    const int          count  = shown ? 6 : 4;
    const std::int32_t inner  = l.screen_h - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t height = std::min<std::int32_t>(RAIL_BTN_H, (inner - (count - 1) * BUTTON_GAP) / count);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(s_drawer); ++i) {
        lv_obj_set_height(lv_obj_get_child(s_drawer, i), height);
    }
}


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
    if (index == CALENDAR_PAGE) {
        show_calendar();
    }
    if (s_handlers.radar != nullptr) {
        s_handlers.radar(index == RADAR_PAGE, page_available(RADAR_PAGE));
    }
    show_guest_presets();
    for (int i = 0; i < PAGE_COUNT; ++i) {
        lv_obj_set_hidden(s_nav_tabs[i], !page_available(i));
        lv_obj_set_hidden(s_pages[i], i != index);
        const bool active = (i == index);
        lv_obj_set_state(s_nav_tabs[i], LV_STATE_CHECKED, active);
        const std::uint32_t ink = active ? theme::text : theme::secondary;
        lv_obj_t *icon = lv_obj_get_child(s_nav_tabs[i], 0);
        if (NAV_ITEMS[i].image != nullptr) {
            lv_obj_set_style_image_recolor(icon, lv_color_hex(ink), 0);
        } else {
            theme::set_text_color(icon, ink);
        }
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

struct InfoCard {
    Subsystem   subsystem;
    const char *title;
    const char *icon;
    const InfoRow *rows;
    int            count;
    Info           summary;
    bool           has_setting;
    Setting        setting;
    const char    *setting_label;
    // A setting that picks between two ways, shown as both. Off first, then on.
    const char    *choice_off = nullptr;
    const char    *choice_on  = nullptr;
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
    {Info::DeskTransport, "Driven over"},  {Info::DeskLink, "Controller"},
    {Info::DeskHeight, "Height"},          {Info::DeskActive, "Standing at"},
    {Info::DeskOne, PRESET_NAMES[0]},      {Info::DeskTwo, PRESET_NAMES[1]},
    {Info::DeskStand, PRESET_NAMES[2]},    {Info::DeskSit, PRESET_NAMES[3]},
    {Info::DeskFive, PRESET_NAMES[4]},     {Info::DeskSix, PRESET_NAMES[5]},
};
constexpr InfoRow RADAR_ROWS[] = {
    {Info::RadarFeed, "Feed"},
    {Info::RadarAircraft, "In range"},
    {Info::RadarRange, "Reach"},
    {Info::RadarSeen, "Last sweep"},
};
constexpr InfoRow MEDIA_ROWS[] = {
    {Info::MediaPlayer, "Playing"},
    {Info::MediaArt, "Artwork"},
    {Info::MediaDecoder, "Decoder"},
};
constexpr InfoRow CALENDAR_ROWS[] = {
    {Info::CalFeeds, "Feeds"},
    {Info::CalEvents, "Ahead"},
    {Info::CalNext, "Next"},
};
constexpr InfoRow SYSTEM_ROWS[] = {
    {Info::SysFirmware, "Firmware"}, {Info::SysBuilt, "Built"},  {Info::SysUptime, "Uptime"},
    {Info::SysRam, "Internal free"}, {Info::SysPsram, "PSRAM free"},
    {Info::SysRamLow, "Low mark"},
};

constexpr InfoCard INFO_CARDS[] = {
    {Subsystem::Network, "Network", LV_SYMBOL_WIFI, NETWORK_ROWS, static_cast<int>(std::size(NETWORK_ROWS)),
     Info::WifiState, false, Setting::Charging, nullptr},
    {Subsystem::HomeAssistant, "Home Assistant", LV_SYMBOL_HOME, HASS_ROWS, static_cast<int>(std::size(HASS_ROWS)),
     Info::HaSocket, false, Setting::Charging, nullptr},
    {Subsystem::Bluetooth, "Bluetooth", LV_SYMBOL_BLUETOOTH, BLUETOOTH_ROWS,
     static_cast<int>(std::size(BLUETOOTH_ROWS)), Info::BleLink, false, Setting::Charging,
     nullptr},
    {Subsystem::Presence, "Presence", LV_SYMBOL_EYE_OPEN, PRESENCE_ROWS, static_cast<int>(std::size(PRESENCE_ROWS)),
     Info::PhoneState, true, Setting::PresenceGate, "Hide pages while away"},
    {Subsystem::Power, "Power", LV_SYMBOL_BATTERY_FULL, POWER_ROWS, static_cast<int>(std::size(POWER_ROWS)),
     Info::PowerCharge, true, Setting::Charging, "Charge the battery"},
    {Subsystem::Desk, "Desk", LV_SYMBOL_UP, DESK_ROWS, static_cast<int>(std::size(DESK_ROWS)), Info::DeskLink,
     true, Setting::DeskBluetooth, "Drive it over", "Wire", "Bluetooth"},
    {Subsystem::Radar, "Radar", LV_SYMBOL_GPS, RADAR_ROWS, static_cast<int>(std::size(RADAR_ROWS)),
     Info::RadarFeed, false, Setting::Charging, nullptr},
    {Subsystem::Media, "Media", LV_SYMBOL_AUDIO, MEDIA_ROWS, static_cast<int>(std::size(MEDIA_ROWS)),
     Info::MediaPlayer, false, Setting::Charging, nullptr},
    {Subsystem::Calendar, "Calendar", LV_SYMBOL_LIST, CALENDAR_ROWS,
     static_cast<int>(std::size(CALENDAR_ROWS)), Info::CalNext, false, Setting::Charging,
     nullptr},
    {Subsystem::System, "System", LV_SYMBOL_SETTINGS, SYSTEM_ROWS, static_cast<int>(std::size(SYSTEM_ROWS)),
     Info::SysUptime, false, Setting::Charging, nullptr},
};
constexpr int INFO_CARD_COUNT = static_cast<int>(std::size(INFO_CARDS));
static_assert(INFO_CARD_COUNT == static_cast<int>(Subsystem::Count), "a tile per subsystem");
constexpr bool cards_in_order()
{
    for (int i = 0; i < INFO_CARD_COUNT; ++i) {
        if (static_cast<int>(INFO_CARDS[i].subsystem) != i) {
            return false;
        }
    }
    return true;
}
static_assert(cards_in_order(), "a tile is indexed by its subsystem");

constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);

constexpr std::int32_t ROW_CARD_H   = 88;
constexpr std::int32_t DIAG_HEADER_H = 56;
constexpr std::int32_t LOG_W        = 800;
constexpr std::int32_t LOG_H        = 520;
constexpr std::int32_t DETAIL_W     = 660;
constexpr std::int32_t DETAIL_ROW_GAP = 2;
constexpr std::int32_t DETAIL_SET_TOP = 14;
constexpr std::int32_t DETAIL_SET_H   = 56;
constexpr std::int32_t HEADER_GAP   = 22;
constexpr std::int32_t DETAIL_MAX_FRAC = 80;
constexpr std::int32_t DETAIL_PAD   = 24;
constexpr std::int32_t DETAIL_ROW_H = 30;
constexpr std::int32_t TILE_DOT     = 14;
constexpr int LOG_LINE_MAX = 24;

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
std::int32_t s_detail_height[INFO_CARD_COUNT] = {};
bool s_tile_long = false;

lv_obj_t *s_setting_value[SETTING_COUNT] = {};
lv_obj_t *s_setting_choice[SETTING_COUNT][2] = {};
bool      s_setting_on[SETTING_COUNT]    = {};

int s_summary_card[INFO_COUNT] = {};

std::optional<ModalOverlay> s_diagnostics;
std::optional<ModalOverlay> s_log_modal;
lv_obj_t                   *s_log_title = nullptr;
lv_obj_t                   *s_log_line[LOG_LINE_MAX] = {};
lv_obj_t                   *s_log_empty = nullptr;
lv_obj_t                   *s_log_pane  = nullptr;
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

std::uint32_t level_colour(char level)
{
    switch (level) {
        case 'E': return theme::red;
        case 'W': return theme::amber;
        case 'D': return theme::secondary;
        default: return theme::text;
    }
}

// Built on first sight rather than at startup: forty labels are a few kilobytes
// of the internal pool the Wi-Fi transport needs, and the log is rarely opened.
void refresh_log()
{
    if (s_log_shown < 0 || s_handlers.log == nullptr) {
        return;
    }
    auto *lines = static_cast<LogLine *>(heap_caps_malloc(
        sizeof(LogLine) * LOG_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (lines == nullptr) {
        return;
    }

    const int count = s_handlers.log(INFO_CARDS[s_log_shown].subsystem, lines, LOG_LINE_MAX);
    for (int i = 0; i < LOG_LINE_MAX; ++i) {
        lv_obj_set_hidden(s_log_line[i], i >= count);
        if (i < count) {
            theme::set_text(s_log_line[i], lines[i].text);
            theme::set_text_color(s_log_line[i], level_colour(lines[i].level));
        }
    }
    lv_obj_set_hidden(s_log_empty, count > 0);
    heap_caps_free(lines);
}

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
    lv_obj_scroll_to_y(s_log_pane, 0, LV_ANIM_OFF);
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
    for (int side = 0; side < 2; ++side) {
        lv_obj_t *choice = s_setting_choice[index][side];
        if (choice != nullptr) {
            const bool picked = (side == 1) == on;
            lv_obj_set_state(choice, LV_STATE_CHECKED, picked);
            theme::set_text_color(lv_obj_get_child(choice, 0), picked ? theme::text : theme::secondary);
        }
    }
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

void choice_clicked_cb(lv_event_t *e)
{
    const int  code  = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    const int  index = code / 2;
    const bool next  = code % 2 == 1;
    if (next == s_setting_on[index]) {
        return;
    }
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

// A tile names a reading and shows it large, as the diagnostics grid does. A
// tile that is a way somewhere or a thing to do is led by its title instead,
// with the explanation under it: the other way round, a description ends up the
// biggest thing on the card.
lv_obj_t *build_tile(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h, const char *icon, const char *title, bool titled = false,
                     const lv_image_dsc_t *image = nullptr)
{
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    theme::style_button(tile, theme::panel_light);
    lv_obj_set_style_pad_all(tile, 20, 0);

    lv_obj_t *glyph = nullptr;
    if (image != nullptr) {
        glyph = lv_image_create(tile);
        lv_image_set_src(glyph, image);
        lv_obj_set_style_image_recolor(glyph, lv_color_hex(theme::primary), 0);
        lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
        lv_obj_set_clickable(glyph, false);
    } else {
        glyph = theme::make_accent_label(tile, icon, fonts::size_28());
    }
    lv_obj_align(glyph, LV_ALIGN_TOP_LEFT, 0, 0);

    const lv_font_t *font = titled ? theme::type_title() : fonts::size_22();
    lv_obj_t *caption     = theme::make_label(tile, title,
                                              titled ? theme::text : theme::secondary, font);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 44, titled ? 0 : 4);
    lv_obj_set_width(caption, w - 96);
    lv_obj_set_height(caption, font->line_height);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS);
    return tile;
}

lv_obj_t *tile_note(lv_obj_t *tile, std::int32_t w, const char *initial)
{
    lv_obj_t *note = theme::make_label(tile, initial, theme::secondary, theme::type_body());
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_width(note, w - 40);
    lv_obj_set_height(note, theme::type_body()->line_height);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_DOTS);
    return note;
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
    lv_obj_set_height(value, font->line_height);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    return value;
}

void build_info_tile(lv_obj_t *parent, int index, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h)
{
    lv_obj_t *tile =
        build_tile(parent, x, y, w, h, INFO_CARDS[index].icon, INFO_CARDS[index].title, false,
                   INFO_CARDS[index].subsystem == Subsystem::Radar      ? &icons::plane_icon
                   : INFO_CARDS[index].subsystem == Subsystem::Calendar ? &icons::calendar_icon
                                                                        : nullptr);
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
        s_detail_height[i] = detail_height(INFO_CARDS[i]);
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

        if (INFO_CARDS[i].has_setting && INFO_CARDS[i].choice_on != nullptr) {
            const int index = static_cast<int>(INFO_CARDS[i].setting);
            lv_obj_t *pair  = lv_obj_create(panel);
            theme::style_panel(pair, theme::panel, 0);
            lv_obj_set_style_bg_opa(pair, LV_OPA_TRANSP, 0);
            lv_obj_set_size(pair, LV_PCT(100), DETAIL_SET_H);
            lv_obj_set_style_margin_top(pair, DETAIL_SET_TOP, 0);
            lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(pair, BUTTON_GAP, 0);
            const char *names[2] = {INFO_CARDS[i].choice_off, INFO_CARDS[i].choice_on};
            for (int side = 0; side < 2; ++side) {
                lv_obj_t *button = lv_button_create(pair);
                lv_obj_set_height(button, LV_PCT(100));
                lv_obj_set_flex_grow(button, 1);
                theme::style_button(button, theme::panel_light);
                theme::fill_accent(button, LV_STATE_CHECKED);
                lv_obj_add_event_cb(
                    button, choice_clicked_cb, LV_EVENT_CLICKED,
                    reinterpret_cast<void *>(static_cast<std::intptr_t>(index * 2 + side)));
                lv_obj_center(theme::make_label(button, names[side], theme::secondary,
                                                fonts::size_20()));
                s_setting_choice[index][side] = button;
            }
            apply_setting(index, s_setting_on[index]);
        } else if (INFO_CARDS[i].has_setting) {
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

    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, 2, 0);
    s_log_pane = pane;

    for (int i = 0; i < LOG_LINE_MAX; ++i) {
        s_log_line[i] = theme::make_label(pane, "", theme::secondary, fonts::size_16());
        lv_obj_set_width(s_log_line[i], width - 28);
        lv_label_set_long_mode(s_log_line[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_hidden(s_log_line[i], true);
    }
    s_log_empty = theme::make_label(pane, "Nothing logged yet", theme::secondary,
                                    fonts::size_16());

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
    lv_obj_t *tile = build_tile(parent, 0, y, w, h, icon, title, true);
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
    const std::int32_t half    = (w - BUTTON_GAP) / 2;

    lv_obj_t *look = build_page_tile(view, tiles_y, w, tile_h, LV_SYMBOL_IMAGE, "Appearance",
                                     show_appearance_cb);
    tile_note(look, w, "Colour, sidebar, orientation, brightness");

    lv_obj_t *diag = build_page_tile(view, tiles_y + pitch, w, tile_h, LV_SYMBOL_LIST,
                                     "Diagnostics", show_diagnostics_cb);
    s_diag_summary = tile_note(diag, w, "");

    lv_obj_t *screen = build_tile(view, 0, tiles_y + 2 * pitch, half, tile_h,
                                  LV_SYMBOL_EYE_CLOSE, "Screen off", true);
    lv_obj_add_event_cb(screen, screen_off_cb, LV_EVENT_CLICKED, nullptr);
    tile_note(screen, half, "Tap anywhere to bring it back");

    lv_obj_t *restart = build_tile(view, half + BUTTON_GAP, tiles_y + 2 * pitch, half, tile_h,
                                   LV_SYMBOL_POWER, "Restart", true);
    lv_obj_add_event_cb(restart, restart_held_cb, LV_EVENT_LONG_PRESSED, nullptr);
    tile_note(restart, half, "Hold to restart");

    s_settings_view = view;
}

void build_appearance_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);
    lv_obj_set_hidden(view, true);
    build_sub_header(view, "Appearance");

    const std::int32_t body_y = DIAG_HEADER_H + BUTTON_GAP;
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

        lv_obj_t *icon = nullptr;
        if (NAV_ITEMS[i].image != nullptr) {
            icon = lv_image_create(tab);
            lv_image_set_src(icon, NAV_ITEMS[i].image);
            lv_obj_set_style_image_recolor(icon, lv_color_hex(theme::secondary), 0);
            lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            icon = theme::make_label(tab, NAV_ITEMS[i].icon, theme::secondary, fonts::size_28());
        }
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

    {
        const Layout cl = layout();
        lv_obj_set_style_pad_all(s_pages[1], PANEL_PAD, 0);
        build_calendar_page(s_pages[1], cl.content_w - 2 * PANEL_PAD,
                            cl.content_h - 2 * PANEL_PAD);
    }
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
constexpr int SPLASH_SEGMENTS = 12;
lv_obj_t   *s_splash_seg[SPLASH_SEGMENTS] = {};
lv_obj_t   *s_splash_about = nullptr;

constexpr const char *SPLASH_ICONS[] = {LV_SYMBOL_HOME, "", LV_SYMBOL_BELL, "",
                                        LV_SYMBOL_SETTINGS};
const lv_image_dsc_t *const SPLASH_IMAGES[] = {nullptr, &icons::calendar_icon, nullptr,
                                               &icons::plane_icon, nullptr};
constexpr int          SPLASH_ICON_COUNT = static_cast<int>(std::size(SPLASH_ICONS));
constexpr std::int32_t SPLASH_CHIP       = 56;
constexpr float        SPLASH_REACH      = 158.0f;
constexpr float        SPLASH_SPREAD     = 68.0f;  // degrees either side of straight up
lv_obj_t    *s_splash_icon[SPLASH_ICON_COUNT] = {};
lv_point_t   s_splash_origin = {};  // the middle of the desktop at its lowest

struct SplashStep {
    const char *key;
    const char *name;
    lv_obj_t   *state;
};
SplashStep s_splash_steps[] = {{"desk", "Desk", nullptr},
                               {"network", "Network", nullptr},
                               {"", "Home Assistant", nullptr}};  // up when splash_done()
constexpr int SPLASH_STEP_COUNT = static_cast<int>(std::size(s_splash_steps));
lv_obj_t   *s_splash_top   = nullptr;
lv_obj_t   *s_splash_leg[2] = {};
lv_timer_t *s_splash_guard = nullptr;
bool        s_splash_up    = false;
lv_timer_t  *s_splash_tick  = nullptr;
std::uint32_t s_splash_start = 0;
bool         s_splash_ready  = false;


constexpr std::int32_t DESK_W     = 300;
constexpr std::int32_t DESK_H     = 160;
constexpr std::int32_t DESK_BAR   = 16;
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

// Driven by the clock alone, so a slow frame does not slow the splash.
constexpr std::uint32_t SPLASH_MS = 12000;

// LVGL redraws whatever a style is set on, changed or not, so each frame sets
// only what moved.
void set_colour_once(lv_obj_t *obj, lv_color_t colour)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), colour)) {
        lv_obj_set_style_bg_color(obj, colour, 0);
    }
}

void set_pos_once(lv_obj_t *obj, std::int32_t x, std::int32_t y)
{
    if (lv_obj_get_x_aligned(obj) != x || lv_obj_get_y_aligned(obj) != y) {
        lv_obj_set_pos(obj, x, y);
    }
}

void splash_frame(std::uint32_t elapsed)
{
    const float run = std::min(static_cast<float>(elapsed) / SPLASH_MS, 1.0f);
    for (int i = 0; i < SPLASH_SEGMENTS; ++i) {
        const float lit = std::clamp(run * SPLASH_SEGMENTS - static_cast<float>(i), 0.0f, 1.0f);
        set_colour_once(s_splash_seg[i],
                        lv_color_mix(lv_color_hex(theme::primary), lv_color_hex(theme::panel),
                                     static_cast<std::uint8_t>(255 * lit)));
    }

    auto phase = [&](float from_ms, float length_ms) {
        return std::clamp((static_cast<float>(elapsed) - from_ms) / length_ms, 0.0f, 1.0f);
    };
    const float rise = 0.5f - 0.5f * std::cos(phase(300.0f, 5200.0f) * 3.14159265f);
    const std::int32_t top =
        DESK_LOW - static_cast<std::int32_t>(std::lround((DESK_LOW - DESK_HIGH) * rise));
    set_pos_once(s_splash_top, 0, top);

    for (int i = 0; i < SPLASH_ICON_COUNT; ++i) {
        const float t    = phase(6000.0f + 1000.0f * static_cast<float>(i), 600.0f);
        const float back = 1.0f + 2.70158f * std::pow(t - 1.0f, 3.0f) +
                           1.70158f * std::pow(t - 1.0f, 2.0f);
        const float angle = (-SPLASH_SPREAD + 2.0f * SPLASH_SPREAD * static_cast<float>(i) /
                                                  (SPLASH_ICON_COUNT - 1)) *
                            3.14159265f / 180.0f;
        const float ox   = static_cast<float>(s_splash_origin.x);
        const float oy   = static_cast<float>(s_splash_origin.y + top - DESK_LOW);
        const float to_x = ox + SPLASH_REACH * std::sin(angle);
        const float to_y = static_cast<float>(s_splash_origin.y + DESK_HIGH - DESK_LOW) -
                           SPLASH_REACH * std::cos(angle);
        static bool landed[SPLASH_ICON_COUNT] = {};
        if (t <= 0.0f || landed[i]) {
            continue;
        }
        landed[i] = t >= 1.0f;
        set_pos_once(s_splash_icon[i],
                     static_cast<std::int32_t>(std::lround(ox + (to_x - ox) * back)) -
                         SPLASH_CHIP / 2,
                     static_cast<std::int32_t>(std::lround(oy + (to_y - oy) * back)) -
                         SPLASH_CHIP / 2);
        const auto scale = static_cast<std::int32_t>(256 * std::min(t * 2.5f, 1.0f));
        if (lv_obj_get_style_transform_scale_x(s_splash_icon[i], LV_PART_MAIN) != scale) {
            lv_obj_set_style_transform_scale(s_splash_icon[i], scale, 0);
        }
        const auto opa = static_cast<lv_opa_t>(255 * std::min(t * 3.0f, 1.0f));
        if (lv_obj_get_style_opa(s_splash_icon[i], LV_PART_MAIN) != opa) {
            lv_obj_set_style_opa(s_splash_icon[i], opa, 0);
        }
    }

    for (lv_obj_t *leg : s_splash_leg) {
        if (lv_obj_get_y_aligned(leg) != top + DESK_BAR) {
            lv_obj_set_y(leg, top + DESK_BAR);
            lv_obj_set_height(leg, DESK_H - DESK_BAR - 12 - top);
        }
    }
}

// Boot takes nine to ten seconds; the splash always takes twelve, one even
// movement rather than a lurch per step, and waits at the end if boot is slower.
constexpr std::uint32_t SPLASH_GUARD_MS = 15000;
bool                    s_splash_leaving = false;

// LVGL draws the screen under the top layer even where the splash covers it, and
// Home Assistant filling the pages in cost a fifth of a second a frame.
constexpr int SPLASH_HIDDEN_MAX = 16;
lv_obj_t     *s_splash_hid[SPLASH_HIDDEN_MAX] = {};

void hide_under_splash()
{
    lv_obj_t *scr  = lv_screen_active();
    int       used = 0;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(scr) && used < SPLASH_HIDDEN_MAX; ++i) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            s_splash_hid[used++] = child;
        }
    }
}

// A cut, not a fade: a full-screen blend takes a quarter of a second a frame here.
void splash_leave()
{
    s_splash_leaving = true;
    for (lv_obj_t *&obj : s_splash_hid) {
        if (obj != nullptr) {
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            obj = nullptr;
        }
    }
    splash_hide(nullptr);
}

void splash_animate(lv_timer_t *)
{
    const std::uint32_t elapsed = lv_tick_elaps(s_splash_start);
    splash_frame(elapsed);
    if (elapsed >= SPLASH_MS + 400 && s_splash_ready && !s_splash_leaving) {
        splash_leave();
    }
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

void splash_mark(int current)
{
    for (int i = 0; i < SPLASH_STEP_COUNT; ++i) {
        lv_obj_t *state = s_splash_steps[i].state;
        theme::set_text(state, i < current ? "Ready" : i == current ? "Starting" : "Waiting");
        theme::set_text_color(state, i < current    ? theme::primary
                                     : i == current ? theme::text
                                                    : theme::secondary);
        lv_obj_set_style_text_opa(state, i > current ? LV_OPA_60 : LV_OPA_COVER, 0);
    }
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
    hide_under_splash();

    constexpr std::int32_t STEPS_W = 360;
    lv_obj_t *panel = lv_obj_create(s_splash);
    lv_obj_set_pos(panel, GAP, GAP);
    lv_obj_set_size(panel, l.screen_w - 2 * GAP, l.screen_h - 2 * GAP);
    theme::style_panel(panel, theme::panel, theme::radius::card);
    lv_obj_set_style_pad_all(panel, PANEL_PAD, 0);
    lv_obj_set_clickable(panel, false);
    const std::int32_t inner_w = l.screen_w - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t inner_h = l.screen_h - 2 * GAP - 2 * PANEL_PAD;
    const std::int32_t hero_h  = inner_h;
    const std::int32_t hero_full_w = inner_w - STEPS_W - GAP;

    lv_obj_t *hero = theme::make_card(panel);
    lv_obj_set_pos(hero, 0, 0);
    lv_obj_set_size(hero, hero_full_w, hero_h);
    lv_obj_set_clickable(hero, false);

    constexpr std::int32_t SEG_W = 30, SEG_H = 10, SEG_GAP = 8;
    const std::int32_t hero_w  = hero_full_w - 2 * theme::space::l;
    const std::int32_t arc_h   = static_cast<std::int32_t>(SPLASH_REACH) + SPLASH_CHIP / 2;
    const std::int32_t block   = arc_h + DESK_H + theme::space::l +
                               theme::type_display()->line_height +
                               theme::type_body()->line_height + theme::space::xl + SEG_H;
    const std::int32_t top     = (hero_h - 2 * theme::space::l - block) / 2 + arc_h;

    // Created before the desk, so they come out from behind it.
    for (int i = 0; i < SPLASH_ICON_COUNT; ++i) {
        s_splash_icon[i] = theme::make_chip(hero, SPLASH_ICONS[i], theme::type_value());
        lv_obj_set_size(s_splash_icon[i], SPLASH_CHIP, SPLASH_CHIP);
        lv_obj_set_style_radius(s_splash_icon[i], SPLASH_CHIP / 2, 0);
        lv_obj_set_style_transform_pivot_x(s_splash_icon[i], SPLASH_CHIP / 2, 0);
        lv_obj_set_style_transform_pivot_y(s_splash_icon[i], SPLASH_CHIP / 2, 0);
        lv_obj_set_style_opa(s_splash_icon[i], LV_OPA_TRANSP, 0);
        lv_obj_t *glyph = lv_obj_get_child(s_splash_icon[i], 0);
        if (SPLASH_IMAGES[i] != nullptr) {
            lv_obj_delete(glyph);
            glyph = lv_image_create(s_splash_icon[i]);
            lv_image_set_src(glyph, SPLASH_IMAGES[i]);
            lv_obj_set_style_image_recolor(glyph, lv_color_hex(theme::primary), 0);
            lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
            lv_obj_center(glyph);
        } else {
            theme::set_text_color(glyph, theme::primary);
            lv_obj_set_style_text_opa(glyph, LV_OPA_COVER, 0);
        }
        lv_obj_set_clickable(s_splash_icon[i], false);
    }
    s_splash_origin = {hero_w / 2, top + DESK_LOW};

    lv_obj_t *desk = lv_obj_create(hero);
    lv_obj_set_size(desk, DESK_W, DESK_H);
    lv_obj_align(desk, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_bg_opa(desk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(desk, 0, 0);
    lv_obj_set_style_pad_all(desk, 0, 0);
    lv_obj_set_clickable(desk, false);
    splash_bar(desk, 8, DESK_H - 12, 88, 12, theme::panel);
    splash_bar(desk, DESK_W - 96, DESK_H - 12, 88, 12, theme::panel);
    s_splash_leg[0] = splash_bar(desk, 44, 0, DESK_BAR, 10, theme::panel);
    s_splash_leg[1] = splash_bar(desk, DESK_W - 44 - DESK_BAR, 0, DESK_BAR, 10, theme::panel);
    s_splash_top = splash_bar(desk, 0, 0, DESK_W, DESK_BAR, theme::panel);
    lv_obj_set_style_radius(s_splash_top, DESK_BAR / 2, 0);
    theme::fill_accent(s_splash_top);

    const std::int32_t title_y = top + DESK_H + theme::space::l;
    lv_obj_align(theme::make_label(hero, "Smart Flexispot", theme::text, theme::type_display()),
                 LV_ALIGN_TOP_MID, 0, title_y);
    lv_obj_align(theme::make_label(hero, "Wouter ten Brinke", theme::secondary, theme::type_body()),
                 LV_ALIGN_TOP_MID, 0, title_y + theme::type_display()->line_height);

    const std::int32_t bar_y = title_y + theme::type_display()->line_height +
                               theme::type_body()->line_height + theme::space::xl;
    lv_obj_t *segments = lv_obj_create(hero);
    lv_obj_set_size(segments, SPLASH_SEGMENTS * (SEG_W + SEG_GAP) - SEG_GAP, SEG_H);
    lv_obj_align(segments, LV_ALIGN_TOP_MID, 0, bar_y);
    lv_obj_set_style_bg_opa(segments, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(segments, 0, 0);
    lv_obj_set_style_pad_all(segments, 0, 0);
    lv_obj_set_clickable(segments, false);
    for (int i = 0; i < SPLASH_SEGMENTS; ++i) {
        s_splash_seg[i] = splash_bar(segments, i * (SEG_W + SEG_GAP), 0, SEG_W, SEG_H,
                                     theme::panel);
        lv_obj_set_style_radius(s_splash_seg[i], SEG_H / 2, 0);
    }

    char about[80];
    std::snprintf(about, sizeof(about), "M5Stack Tab5  \xc2\xb7  build %s",
                  esp_app_get_description()->version);
    s_splash_about = theme::make_label(hero, about, theme::secondary, theme::type_label());
    lv_obj_set_style_text_opa(s_splash_about, LV_OPA_60, 0);
    lv_obj_align(s_splash_about, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_splash_about, true);
    lv_obj_add_event_cb(
        s_splash,
        [](lv_event_t *) {
            lv_obj_set_hidden(s_splash_about, !lv_obj_has_flag(s_splash_about, LV_OBJ_FLAG_HIDDEN));
        },
        LV_EVENT_CLICKED, nullptr);

    const std::int32_t step_h = (inner_h - (SPLASH_STEP_COUNT - 1) * GAP) / SPLASH_STEP_COUNT;
    const lv_image_dsc_t *const STEP_ICONS[SPLASH_STEP_COUNT] = {&icons::desk_icon,
                                                                 &icons::wifi_icon, nullptr};
    for (int i = 0; i < SPLASH_STEP_COUNT; ++i) {
        lv_obj_t *card = theme::make_card(panel);
        lv_obj_set_pos(card, hero_full_w + GAP, i * (step_h + GAP));
        lv_obj_set_size(card, STEPS_W, step_h);
        lv_obj_set_clickable(card, false);

        lv_obj_t *chip = theme::make_chip(card, STEP_ICONS[i] != nullptr ? "" : LV_SYMBOL_HOME,
                                          fonts::size_28());
        lv_obj_set_clickable(chip, false);
        if (STEP_ICONS[i] != nullptr) {
            lv_obj_t *image = lv_image_create(chip);
            lv_image_set_src(image, STEP_ICONS[i]);
            lv_obj_set_style_image_recolor(image, lv_color_hex(theme::secondary), 0);
            lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
            lv_obj_set_style_image_opa(image, theme::mark_opa, 0);
            lv_obj_center(image);
        }

        lv_obj_t *text = lv_obj_create(card);
        lv_obj_set_size(text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_align(text, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(text, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text, 0, 0);
        lv_obj_set_style_pad_all(text, 0, 0);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(text, theme::space::xs, 0);
        lv_obj_set_clickable(text, false);
        theme::make_label(text, s_splash_steps[i].name, theme::secondary, theme::type_label());
        s_splash_steps[i].state = theme::make_label(text, "", theme::text, theme::type_title());
    }
    splash_mark(0);
    splash_frame(0);
    s_splash_start = lv_tick_get();
    s_splash_tick  = lv_timer_create(splash_animate, 16, nullptr);

    s_splash_guard = lv_timer_create(splash_expired, SPLASH_GUARD_MS, nullptr);
    lv_timer_set_repeat_count(s_splash_guard, 1);
}

void build_screen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(theme::background), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);

    for (lv_indev_t *dev = lv_indev_get_next(nullptr); dev != nullptr;
         dev = lv_indev_get_next(dev)) {
        lv_indev_add_event_cb(dev, wake_on_touch, LV_EVENT_PRESSED, nullptr);
    }

    create_rail(scr);
    create_content(scr);
    // Development: hands a picture of the screen to tools/screenshot.py a little
    // after boot. SHOT_PAGE picks what to look at; -1 leaves the panel alone.
    // It holds the LVGL lock for several seconds, so it is off unless wanted.
    if (SHOT_ENABLED) {
        // One page per tick rather than all at once: a tab's colour eases in,
        // and a picture taken straight after the switch shows the old tab lit.
        // Pages hidden while the phone is away are the ones most often worth
        // looking at, so the gate is lifted for as long as the pictures take.
        lv_timer_t *shot = lv_timer_create([](lv_timer_t *timer) {
            // Only the pages being worked on: every one adds about a minute.
            static const int PAGES[] = {CALENDAR_PAGE};
            static int       step    = -1;
            static bool      gated   = false;
            if (step >= 0) {
                screenshot();
            } else {
                gated           = s_presence_gate;
                s_presence_gate = false;
            }
            if (++step < static_cast<int>(std::size(PAGES))) {
                select_page(PAGES[step]);
                if (SHOT_DRAWER) {
                    place_drawer(DRAWER_W);
                }
                lv_timer_set_period(timer, 2000);
                return;
            }
            s_presence_gate = gated;
            select_page(0);
            lv_timer_delete(timer);
        }, 25000, nullptr);
        (void)shot;
    }
    create_drawer(scr);  // after the content, so it overlays it when open
    lv_obj_move_foreground(s_rail);  // and under the rail, which it slides out from
    create_notice_card();
    build_splash();  // last, so it covers everything until startup finishes

}

}  // namespace

esp_err_t splash_step(const char *label)
{
    ESP_RETURN_ON_FALSE(s_splash != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (!s_splash_up) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    for (int i = 0; i < SPLASH_STEP_COUNT; ++i) {
        if (label != nullptr && std::strcmp(label, s_splash_steps[i].key) == 0) {
            splash_mark(i + 1);
        }
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t splash_done()
{
    ESP_RETURN_ON_FALSE(s_splash != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    if (s_splash_up) {
        ESP_LOGI(TAG, "splash: ready after %u ms",
                 static_cast<unsigned>(lv_tick_elaps(s_splash_start)));
        s_splash_up    = false;
        s_splash_ready = true;
        splash_mark(SPLASH_STEP_COUNT);
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
    fonts::init();
    theme::init_accents();
    if (accent != 0) {
        theme::set_primary(accent);
    }
    s_rail_right         = rail_right;
    s_flipped            = flipped;
    s_handlers           = handlers;
    s_initial_brightness = initial_brightness;
    build_screen();
    lv_refr_now(nullptr);
    lvgl_port_unlock();
    return ESP_OK;
}

const char *preset_name(int index)
{
    return index >= 0 && index < kPresetCount ? PRESET_NAMES[index] : deskproto::kBetween;
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
        lv_obj_set_state(obj, LV_STATE_DISABLED, !available);
        lv_obj_t *label = lv_obj_get_child(obj, 0);
        if (label != nullptr) {
            theme::set_text_color(label, available ? theme::text : theme::disabled_ink);
        }
        lv_obj_set_clickable(obj, available);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_media(const char *source, const char *title, const char *artist, const char *state,
                    bool playing)
{
    ESP_RETURN_ON_FALSE(s_media_card != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    const bool has_track = title != nullptr && title[0] != '\0';
    s_media_off = state == nullptr || std::strcmp(state, "OFF") == 0 || std::strcmp(state, "--") == 0;
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
        theme::center_ink(text);
        lv_obj_set_state(chip, LV_STATE_CHECKED, on);
        theme::set_text_color(text, on ? theme::text : theme::secondary);
        lv_obj_set_style_text_opa(text, on ? LV_OPA_COVER : theme::mark_opa, 0);
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
    ESP_RETURN_ON_FALSE(s_phone_icon != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");

    const bool here = has_key && present;
    lv_image_set_src(s_phone_icon, here ? &icons::phone_icon : &icons::phone_off_icon);

    const bool known = has_key && ever_seen;
    if (known != s_presence_known || here != s_present) {
        s_presence_known = known;
        s_present        = here;
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
    static int last = -1;
    if (last == (wifi ? 1 : 0)) {
        return ESP_OK;
    }
    last = wifi ? 1 : 0;
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_image_set_src(s_wifi_icon, wifi ? &icons::wifi_icon : &icons::wifi_off_icon);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_battery(bool present, int percent, bool charging)
{
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

    const int card = s_summary_card[index];
    if (card >= 0) {
        theme::set_text(s_tile_value[card], text);
    }

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_calendar()
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    show_calendar();
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

esp_err_t set_media_hold_preset(int preset)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    s_media_hold = preset;
    if (preset >= 0 && s_media_panel.has_value()) {
        s_media_panel->close();  // what it controlled is no longer on the card
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

esp_err_t set_screen(bool on)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    s_screen_on         = on;
    s_notice_lit_screen = false;
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_setting(Setting setting, bool on)
{
    const int index = static_cast<int>(setting);
    ESP_RETURN_ON_FALSE(index >= 0 && index < SETTING_COUNT, ESP_ERR_INVALID_ARG, TAG, "setting %d",
                        index);
    ESP_RETURN_ON_FALSE(s_setting_value[index] != nullptr || s_setting_choice[index][0] != nullptr,
                        ESP_ERR_INVALID_STATE, TAG,
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
