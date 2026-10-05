#pragma once

#include <ctime>

// What the files of the screen share with each other. Nothing outside the
// ui component includes this; ui.h is the interface.

#include "ui.h"
#include "media_model.h"

#include "board.h"
#include "media.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "modal_overlay.h"
#include "views.h"
#include "calendar_page.h"
#include "focus_view.h"
#include "screenshot.h"
#include "radar_page.h"
#include "icons.h"
#include "theme.h"
#include "units.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <iterator>
#include <optional>
#include <utility>

namespace ui::detail {
constexpr char TAG[] = "ui";

constexpr std::uint32_t LOCK_TIMEOUT_MS = 500;

// Sized from the real display rather than with percentages: LV_PCT() returns an
// encoded sentinel, so LV_PCT(100) - something lays out as nonsense.
constexpr std::int32_t GAP        = 16;
// The frame: a dock at one side with the tabs and the desk, a row along the
// top with the time and what goes on elsewhere, and the page in the rest.
constexpr std::int32_t DOCK_W      = 112;
constexpr std::int32_t TOP_H       = 72;   // the control bar's slots, centred over the pages
constexpr std::int32_t CONTENT_Y   = 92;   // the page under it
constexpr std::int32_t BAR_GAP     = 10;   // between the control bar's slots
constexpr std::int32_t FROM_DOCK   = 24;   // a card unfolding from the dock, from its edge

constexpr std::int32_t PANEL_PAD  = 16;
constexpr std::int32_t BUTTON_GAP = 16;

struct Layout {
    std::int32_t screen_w;
    std::int32_t screen_h;
    std::int32_t content_x;
    std::int32_t content_y;
    std::int32_t content_w;
    std::int32_t content_h;
    std::int32_t dock_x;
    bool         dock_right;  // the dock at the right, as the panel is bolted on the desk's right
};

constexpr int          NOTIFY_QUEUE_LEN  = 4;

struct Notice {
    char source[24];
    char title[64];
    char message[192];
    Level level;
    int   timeout_ms;
};

// What the screen calls each preset. Everything off the screen says Preset 1 to 6.
// ui_presets.h, which git ignores, can name them otherwise.
#if __has_include("ui_presets.h")
#include "ui_presets.h"
#else
constexpr const char *PRESET_NAMES[kPresetCount] = {
    "Preset 1", "Ultra low", "Stand", "Sit", "Sit 2", "Stand 2",
};
#endif



constexpr std::uint32_t PAUSE_SETTLE_MS = 1500;

// The pages, in the order the dock's tabs are made.
constexpr int HOME_PAGE     = 0;
constexpr int RADAR_PAGE    = 1;
constexpr int CALENDAR_PAGE = 2;
constexpr int SETUP_PAGE    = 3;  // opened from the control bar's status
constexpr int PAGE_COUNT    = 4;


constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);

// A choice card offers one way or the other; orientation offers each Orientation.

// The focus plan the panel shows until the timer reports one, and the limits
// the Setup steppers keep it to, which the fullscreen timer is sized for.
constexpr int FOCUS_WORK_MIN_DEFAULT       = 25;
constexpr int FOCUS_BREAK_MIN_DEFAULT      = 5;
constexpr int FOCUS_LONG_BREAK_MIN_DEFAULT = 20;
constexpr int FOCUS_ROUNDS_DEFAULT         = 4;
constexpr int FOCUS_WORK_MIN_MAX           = 90;
constexpr int FOCUS_ROUNDS_MAX             = 8;

constexpr std::int32_t ROW_CARD_H   = 88;
constexpr std::int32_t DIAG_HEADER_H = 56;
constexpr std::int32_t HEADER_GAP   = 22;
constexpr std::int32_t DETAIL_PAD   = 24;

// A modal's title sits this far down, level with its close button.
constexpr std::int32_t MODAL_TITLE_Y = 6;
inline std::int32_t modal_body_y()
{
    return ModalOverlay::header_height() + HEADER_GAP;
}

// A row card: a full-width strip with an icon, then whatever the row holds.
constexpr std::int32_t ROW_CARD_PAD_X = 22;
constexpr std::int32_t ROW_CARD_GAP   = 18;

extern bool s_dock_right;
extern Orientation s_orientation;
extern Handlers s_handlers;
extern bool s_notice_lit_screen;
extern int s_initial_brightness;
extern lv_obj_t *s_dock;
extern lv_obj_t *s_content;
extern lv_obj_t *s_notice_scrim;
extern lv_obj_t *s_notice_card;
extern lv_obj_t *s_preset_buttons[kPresetCount];
extern int s_page;
extern std::atomic<bool> s_setup_visible;
extern bool s_presence_gate;
/** The phone away with the gate on: the owner's pages, and what shows of them elsewhere, are away. */
bool owner_away();
extern lv_obj_t *s_settings_view;
extern lv_obj_t *s_appearance_view;
extern lv_obj_t *s_diag_view;

/** The focus plan as the timer has it, onto the settings that change it. */
void paint_focus_plan(const Focus &focus);

/** An update arriving or ready: the control bar's icon, the dot on Setup and the
 *  tile that installs it. Each with the LVGL lock held. */
void paint_update_icon(const UpdateState &state);
/** The symbol for what is updating: the screen or companion arriving, or going on. */
const lv_image_dsc_t *update_icon(const UpdateState &state);
void paint_setup_dot(bool ready);
void paint_update_tile(const UpdateState &state);
extern lv_obj_t *s_diag_summary;
extern lv_obj_t *s_volume_value;
extern lv_obj_t *s_volume_slider;
extern const Card *s_cards;
extern int s_card_count;

Layout layout();
void set_screen_state(bool on);
void screen_off_cb(lv_event_t *);
void register_desk_control(lv_obj_t *obj);

/** Stand and Sit as two small chips at x, y over a fullscreen view's root,
 *  which covers the dock; lit as the dock's are. */
inline constexpr int kDeskShortcutButtons = 4;  // Stand and Sit: the dock's, and over the focus, music and cinema views
lv_obj_t *add_desk_shortcuts(lv_obj_t *root, std::int32_t x, std::int32_t y,  // the button
                             std::uint32_t chip_colour);
void paint_desk_shortcuts();
/** A ring in the accent round a preset's button, breathing while the desk is on its way there. */
void show_desk_travel(lv_obj_t *obj, bool travelling);  // after the preset the desk is at changes
void show_next_notice();
void paint_notice_corner();  // when the notice on show came, and how many wait
void create_notice_card();
/** The screen going dark by itself, and lighting again: see screen_rules.h. */
void start_screen_schedule();
/** The notice over the pages or a fullscreen view, the dock left clear. */
void place_notice();
/** The dock: the tabs, Stand and Sit, the height that folds the desk out. */
void create_dock(lv_obj_t *parent);
lv_obj_t *dock_tabs();  // the column the tabs go in
/** After the last tab: Home goes under the desk, the timer under the tabs. */
void dock_tabs_done(lv_obj_t *home);
/** The desk folded out beside the dock: its height, every preset, up and down. */
void create_desk_card(lv_obj_t *parent);
void open_desk_card(bool open);
void place_desk_card();
/** The row along the top of the pages: what goes on elsewhere, the battery, the status, the time. */
void create_top_bar(lv_obj_t *parent);
void place_top_bar();
/** The control bar's slots for what plays, the heating and the lights, and the
 *  cards they drop, kept over the scrim by `bar` while one is out. */
void build_bar_slots(lv_obj_t *bar);
void build_bar_cards(lv_obj_t *screen, lv_obj_t *bar);
/** The laptops' Claude Code sessions, beside the lights while any is about, and their card. */
void build_claude_slot(lv_obj_t *bar);
void build_claude_card(lv_obj_t *screen, lv_obj_t *bar);
/** The thermostat, `w` by `h` at the top left of `parent`: the heating slot's card. */
void build_thermostat_card(lv_obj_t *parent, std::int32_t w, std::int32_t h);
void place_for_side();  // the dock, the page, the bar and its cards, after the dock changes side
std::uint32_t level_ink(Level level);
/** The HK Citation One the control bar's covers show while nothing plays,
 *  `side` square, painted once at the size first asked for. */
const lv_image_dsc_t *speaker_picture(std::int32_t side);
void write_clock(lv_obj_t *label, int seconds);
void build_home_page(lv_obj_t *page);

// The presets the dock's Stand and Sit send the desk to.
constexpr int STAND_PRESET     = 2;
constexpr int SIT_PRESET       = 3;
constexpr int ULTRA_LOW_PRESET = 1;  // Preset 2, as the cinema view sends the desk down

// The favourites' picker, when there are any, as the music view opens it.
void open_favourites();
void build_favourites(lv_obj_t *screen);  // over the pages and the music view, which open it

// Jellyfin fullscreen: the film, its controls, the desk and the lights.
void build_cinema(lv_obj_t *screen);
void open_cinema();
/** How long the open film leaves the screen lit untouched: 0 with none open,
 *  screen_rules::kNever while it keeps it lit. */
std::int64_t cinema_dark_after();
void build_music(lv_obj_t *screen);
void open_music();
// The time beside a fullscreen view's chip back, faded with its other buttons,
// and the focus timer beside it while it runs, unless the view is the timer's.
struct ViewClock {
    lv_obj_t *label;
    lv_obj_t *badge;  // null without one
    lv_obj_t *power;  // the battery, shown while unplugged
};
ViewClock add_view_clock(ViewId view, lv_obj_t *root, lv_obj_t *chip, bool focus_badge = true);
// Stand and Sit, the time and the way back, over a fullscreen view, and any
// quick actions of the view's own after Stand and Sit.
struct Chrome {
    ViewId       view;
    lv_obj_t    *root;
    lv_obj_t    *close;
    lv_obj_t    *desk;    // its chips are its children
    lv_obj_t    *badge;   // the focus timer's, or null
    std::int32_t next_x;  // where a quick action goes
};
Chrome    add_fullscreen_chrome(ViewId view, lv_obj_t *root, lv_event_cb_t on_close, bool focus_badge = true);
lv_obj_t *add_chrome_chip(Chrome &chrome, const lv_image_dsc_t *icon, lv_event_cb_t on_click);
void show_guest_presets();
void select_page(int index);
/** Setup from the control bar's status, and back to where it was opened from. */
void toggle_setup();
/** The owner's pages come and go with their phone, and with the setting that hides them. */
void follow_pages();
std::uint32_t info_ink(Level level);
/** The diagnostics page follows the diagnostics model. */
void follow_diagnostics();
void refresh_diag_summary();
void show_diagnostics_cb(lv_event_t *);
void show_appearance_cb(lv_event_t *);
void show_settings_cb(lv_event_t *);
void show_behaviour_cb(lv_event_t *);
void show_log_cb(lv_event_t *);
void apply_splash();

/** For updates kept outside ui.cpp's slots: asks the LVGL task to look. */
void request_apply();
void restart_held_cb(lv_event_t *);
void volume_changed_cb(lv_event_t *e);
lv_obj_t *build_tile(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h, const char *icon, const char *title, bool titled = false,
                     const lv_image_dsc_t *image = nullptr);
lv_obj_t *tile_note(lv_obj_t *tile, std::int32_t w, const char *initial);
void build_info_tile(lv_obj_t *parent, int index, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h);
void build_detail_overlay(lv_obj_t *parent);
void build_log_overlay(lv_obj_t *parent);
/** A row card at y with its icon; clickable when clicked is given. */
lv_obj_t *build_row_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                         lv_event_cb_t clicked);
void write_percent(lv_obj_t *label, int percent);
lv_obj_t *build_slider_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                            const char *title, int value, int low, lv_event_cb_t changed,
                            lv_obj_t **out_value);
void build_settings_page(lv_obj_t *page);
void create_content(lv_obj_t *parent);
/** The pages, one a call; false after the last. */
bool build_next_page();
void build_splash();
/** Hides what has been put on the screen since, until the splash leaves. */
void keep_under_splash();

/** The time the screen shows: the real one, or running on from the clock
 *  pinned by pin_clock(), so what it says agrees with the top bar. */
std::time_t wall_now();

}  // namespace ui::detail
