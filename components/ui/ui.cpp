#include "ui.h"

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "segment_display.h"
#include "theme.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>

namespace ui {
namespace {

constexpr char TAG[] = "ui";

constexpr std::uint32_t LOCK_TIMEOUT_MS = 100;

// Everything is sized from the real display rather than with percentages:
// LV_PCT() returns an encoded sentinel, so LV_PCT(100) - something is not a
// width, it is nonsense that silently lays out wrong.
constexpr std::int32_t RAIL_W      = 330;
constexpr std::int32_t NAV_H       = 92;
constexpr std::int32_t RAIL_BTN_H  = 124;
constexpr std::int32_t GAP        = 16;
constexpr std::int32_t EDGE_GAP   = 0;   // navigation sits on the bottom edge
constexpr std::int32_t NAV_GAP    = 10;  // between the page and the bar

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

MoveHandler       s_on_move       = nullptr;
PresetHandler     s_on_preset     = nullptr;
BrightnessHandler s_on_brightness = nullptr;
int               s_initial_brightness = 80;

std::optional<SegmentDisplay> s_height;
lv_obj_t *s_wifi_icon     = nullptr;
lv_obj_t *s_wifi_slash    = nullptr;
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
    s_notice_card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_notice_card, NOTIFY_W, NOTIFY_H);
    lv_obj_align(s_notice_card, LV_ALIGN_CENTER, 0, 0);
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
    lv_obj_set_style_text_font(s_notice_title, &lv_font_montserrat_28, 0);
    // Explicit: the card is dark and the inherited theme colour is not.
    lv_obj_set_style_text_color(s_notice_title, lv_color_hex(theme::text), 0);

    s_notice_body = lv_label_create(s_notice_card);
    lv_obj_set_width(s_notice_body, NOTIFY_W - 80);
    lv_label_set_long_mode(s_notice_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_notice_body, LV_ALIGN_TOP_LEFT, 24, 76);
    lv_obj_set_style_text_color(s_notice_body, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_text_font(s_notice_body, &lv_font_montserrat_28, 0);

    s_notice_timer = lv_timer_create(notice_timeout_cb, NOTIFY_DEFAULT_MS, nullptr);
    lv_timer_pause(s_notice_timer);
}

// --- controls --------------------------------------------------------------

void move_event_cb(lv_event_t *e)
{
    if (s_on_move == nullptr) {
        return;
    }
    const auto direction =
        static_cast<Move>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_on_move(lv_event_get_code(e) == LV_EVENT_PRESSED ? direction : Move::Stop);
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
                                       &lv_font_montserrat_48);
    lv_obj_set_size(btn, w, h);

    auto *user_data = reinterpret_cast<void *>(
        static_cast<std::intptr_t>(std::to_underlying(direction)));
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_RELEASED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESS_LOST, user_data);
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
    return btn;
}

void preset_clicked_cb(lv_event_t *e)
{
    if (s_on_preset != nullptr) {
        s_on_preset(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))),
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

    s_clock_label = theme::make_label(strip, "--:--", theme::text, &lv_font_montserrat_28);
    lv_obj_align(s_clock_label, LV_ALIGN_LEFT_MID, 0, 0);

    s_wifi_icon = theme::make_label(strip, LV_SYMBOL_WIFI, theme::text, &lv_font_montserrat_22);
    lv_obj_align(s_wifi_icon, LV_ALIGN_RIGHT_MID, 0, 0);

    // A slash over the glyph, rather than a second icon: LVGL has no
    // wifi-off symbol and this reads instantly.
    s_wifi_slash = theme::make_label(strip, "/", theme::text, &lv_font_montserrat_32);
    lv_obj_align_to(s_wifi_slash, s_wifi_icon, LV_ALIGN_CENTER, 0, -2);

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
    theme::make_label(readout, "CM", theme::secondary, &lv_font_montserrat_22);

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
    }
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
};
constexpr NavItem NAV_ITEMS[PAGE_COUNT] = {
    {LV_SYMBOL_HOME, "Home"},   {LV_SYMBOL_LIST, "Stats"}, {LV_SYMBOL_BELL, "Alerts"},
    {LV_SYMBOL_WIFI, "Network"}, {LV_SYMBOL_SETTINGS, "Setup"},
};

void select_page(int index)
{
    for (int i = 0; i < PAGE_COUNT; ++i) {
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
    if (s_on_brightness == nullptr) {
        return;
    }
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    s_on_brightness(static_cast<int>(lv_slider_get_value(slider)));
}

// Stand-ins, so the navigation can be seen working before the screens behind
// it exist. Each says what it is for rather than pretending to hold data.
void build_placeholder_page(lv_obj_t *page, const char *title, const char *blurb)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 14, 0);
    theme::make_label(page, title, theme::text, &lv_font_montserrat_32);
    theme::make_label(page, blurb, theme::secondary, &lv_font_montserrat_20);
}

void build_settings_page(lv_obj_t *page)
{
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, 20, 0);

    lv_obj_t *caption = lv_label_create(page);
    lv_label_set_text_static(caption, "Brightness");
    lv_obj_set_style_text_color(caption, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_28, 0);

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
                                           &lv_font_montserrat_28);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -12);
        lv_obj_t *caption = theme::make_label(tab, NAV_ITEMS[i].caption, theme::secondary,
                                              &lv_font_montserrat_16);
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

    build_placeholder_page(s_pages[0], "Home", "The desk lives on the rail, always to hand.");
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

esp_err_t init(MoveHandler on_move, PresetHandler on_preset, BrightnessHandler on_brightness,
               int initial_brightness)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    s_on_move            = on_move;
    s_on_preset          = on_preset;
    s_on_brightness      = on_brightness;
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
