#pragma once

// What the files of the screen share with each other. Nothing outside the
// ui component includes this; ui.h is the interface.

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
#include "views.h"
#include "calendar_page.h"
#include "focus_page.h"
#include "screenshot.h"
#include "radar_page.h"
#include "segment_display.h"
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
constexpr std::int32_t EDGE_GAP   = 0;   // navigation sits on the bottom edge
constexpr std::int32_t RAIL_W      = 330;
// The rail is a panel like the content beside it, set in by the same gap on its
// outer sides rather than running flush to the edge of the glass.
constexpr std::int32_t RAIL_CARD_W = RAIL_W - GAP;
constexpr std::int32_t NAV_H       = 92;
constexpr std::int32_t RAIL_BTN_H  = 124;

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

constexpr int          NOTIFY_QUEUE_LEN  = 4;

struct Notice {
    char source[24];
    char title[64];
    char message[192];
    Level level;
    int   timeout_ms;
};

// What the screen calls each preset. Everything off the screen says Preset 1 to 6.
constexpr const char *PRESET_NAMES[kPresetCount] = {
    "Preset 1", "Ultra low", "Stand", "Sit", "Sit 2", "Stand 2",
};

constexpr std::int32_t DRAWER_W  = 340;

constexpr int   DIAL_SCALE     = 10;
constexpr float DEFAULT_STEP_C = 0.5f;

struct Pill {
    lv_obj_t *root   = nullptr;
    lv_obj_t *dot    = nullptr;
    lv_obj_t *column = nullptr;
    lv_obj_t *label  = nullptr;
    lv_obj_t *value  = nullptr;
    bool      shown  = false;
};
struct LightButton {
    lv_obj_t *root  = nullptr;
    lv_obj_t *name  = nullptr;
    lv_obj_t *state = nullptr;
};

struct TextBox {
    std::int32_t x;
    std::int32_t w;
};
constexpr int PROGRESS_SCALE   = 10;  // bar units per second

constexpr std::uint32_t PAUSE_SETTLE_MS = 1500;

// The tabs, in the order they stand along the bottom.
constexpr int HOME_PAGE     = 0;
constexpr int RADAR_PAGE    = 1;
constexpr int CALENDAR_PAGE = 2;
constexpr int FOCUS_PAGE    = 3;
constexpr int SETUP_PAGE    = 4;
constexpr int PAGE_COUNT    = 5;


constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);

// A choice card offers one way or the other; orientation offers each Orientation.
constexpr int CHOICE_COUNT      = 2;
constexpr int ORIENTATION_COUNT = static_cast<int>(Orientation::Auto) + 1;

// The focus plan the panel shows until the timer reports one, and the limits
// the Setup steppers keep it to, which the focus page is sized for.
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

extern bool s_rail_right;
extern Orientation s_orientation;
extern Handlers s_handlers;
extern bool s_screen_on;
extern bool s_notice_lit_screen;
extern int s_initial_brightness;
extern std::optional<SegmentDisplay> s_height;
extern lv_obj_t *s_rail;
extern lv_obj_t *s_content;
extern lv_obj_t *s_clock_box;
extern lv_obj_t *s_side_buttons[CHOICE_COUNT];
extern lv_obj_t *s_flip_buttons[ORIENTATION_COUNT];
extern lv_obj_t *s_wifi_icon;
extern lv_obj_t *s_phone_icon;
extern lv_obj_t *s_clock_hours;
extern lv_obj_t *s_clock_colon;
extern lv_obj_t *s_clock_minutes;
extern bool s_clock_known;
extern Notice s_notice_queue[NOTIFY_QUEUE_LEN];
extern int s_notice_count;
extern lv_obj_t *s_notice_scrim;
extern lv_obj_t *s_notice_card;
extern lv_obj_t *s_preset_buttons[kPresetCount];
extern bool s_preset_active[kPresetCount];
extern lv_obj_t *s_drawer;
extern lv_obj_t *s_dial;
extern lv_obj_t *s_dial_current;
extern lv_obj_t *s_dial_target;
extern lv_obj_t *s_dial_mode;
extern lv_obj_t *s_dial_toggles[kDialToggleCount];
extern float s_dial_step;
extern bool s_dial_dragging;
extern Pill s_pills[kPillCount];
extern lv_obj_t *s_lights_button;
extern lv_obj_t *s_lights_name;
extern lv_obj_t *s_lights_state;
extern lv_obj_t *s_bulbs[kLightCount];
extern bool s_light_on[kLightCount];
extern bool s_lights_on;
extern LightButton s_lights[kLightCount];
extern lv_obj_t *s_media_card;
extern int s_media_hold;
extern bool s_media_off;
extern lv_obj_t *s_media_frame;
extern lv_obj_t *s_media_art;
extern lv_obj_t *s_media_source;
extern lv_obj_t *s_media_title;
extern lv_obj_t *s_media_artist;
extern lv_image_dsc_t s_art_dsc[2];
extern int s_art_slot;
extern TextBox s_card_with_art, s_card_bare;
extern bool s_has_art;
extern int s_position_s;
extern int s_duration_s;
extern bool s_media_playing;
extern TickType_t s_position_at;
extern lv_timer_t *s_pause_timer;
extern bool s_playing_shown;
extern bool s_has_track_shown;
extern bool s_present;
extern int s_page;
extern std::atomic<bool> s_setup_visible;
extern bool s_presence_gate;
extern lv_obj_t *s_brightness_value;
extern lv_obj_t *s_settings_view;
extern lv_obj_t *s_appearance_view;
extern lv_obj_t *s_diag_view;

/** The focus plan as the timer has it, onto the settings that change it. */
void paint_focus_plan(const Focus &focus);

/** An update arriving or ready: the rail's icon, the dot on Setup and the
 *  tile that installs it. Each with the LVGL lock held. */
void paint_update_icon(const UpdateState &state);
/** The rail's symbol for what is updating: the screen or companion arriving, or going on. */
const lv_image_dsc_t *update_icon(const UpdateState &state);
void paint_setup_dot(bool ready);
void paint_update_tile(const UpdateState &state);
extern lv_obj_t *s_diag_summary;
void apply_glance(int index, const char *value);
extern lv_obj_t *s_volume_value;
extern lv_obj_t *s_volume_slider;
extern const Card *s_cards;
extern int s_card_count;
extern lv_obj_t *s_tile_value[kMaxCards];
extern lv_obj_t *s_tile_dot[kMaxCards];
extern Level s_card_level[kMaxCards];
extern lv_obj_t *s_row_value[kMaxCards][kMaxRows];

Layout layout();
void set_screen_state(bool on);
void screen_off_cb(lv_event_t *);
void register_desk_control(lv_obj_t *obj);

/** Stand and Sit as two small chips at x, y over a fullscreen view's root,
 *  which covers the rail; lit as the rail's are. */
inline constexpr int kDeskShortcutButtons = 8;  // over the radar, focus, music and cinema views
lv_obj_t *add_desk_shortcuts(lv_obj_t *root, std::int32_t x, std::int32_t y,  // what holds them
                             std::uint32_t chip_colour);
void paint_desk_shortcuts();  // after the preset the desk is at changes
void show_next_notice();
void paint_notice_corner();  // when the notice on show came, and how many wait
void create_notice_card();
void create_rail(lv_obj_t *parent);
void place_drawer(std::int32_t width);
void create_drawer(lv_obj_t *parent);
void paint_choice(lv_obj_t *const buttons[2], bool second);
void paint_pick(lv_obj_t *const *buttons, int count, int picked);
void paint_side_buttons();
void apply_rail_side(bool right);
void write_temperature(lv_obj_t *label, float celsius, bool with_unit);
void paint_dial(Hvac state);
void reflow_pills();
std::uint32_t level_ink(Level level);
void paint_bulbs();
void paint_light(lv_obj_t *root, lv_obj_t *name, lv_obj_t *state, bool on);
void layout_media_text();
/** The speaker drawn in the card's cover frame while nothing plays. */
void show_speaker_face(bool shown);
/** Where the card's text starts while idle, to sit level with the speaker. */
std::int32_t idle_media_text_top();
/** The HK Citation One the card shows then, `side` square, painted once. */
const lv_image_dsc_t *speaker_picture(std::int32_t side);
void apply_pick(int index, const char *name);
void apply_pick_art(int index, const void *pixels);
void apply_media_segments(const MediaSegment *segments, int count);
void apply_media_seeks(bool seeks);
void apply_media_remote(bool remote);
/** Whether the player takes pause, seek and skip from here. */
bool media_remote();
void apply_media_subtitles(bool available, bool shown);
void write_clock(lv_obj_t *label, int seconds);
void apply_playing(bool playing);
void cancel_pause_settle();
void pause_settled(lv_timer_t *);
void build_home_page(lv_obj_t *page);

// The presets the rail's Stand and Sit buttons send the desk to.
constexpr int STAND_PRESET     = 2;
constexpr int SIT_PRESET       = 3;
constexpr int ULTRA_LOW_PRESET = 1;  // Preset 2, as the cinema view sends the desk down

// What plays on the media card, for the cinema view to show and control too.
int         media_position_now();     // seconds, carried forward while it plays
int         media_position_ms_now();  // the same in milliseconds, for a bar that glides
void        open_favourites();        // the favourites' picker, when there are any
void        media_seek_by(int delta_s);
void        media_toggle_play();
const char *media_skip_text();     // what the skip button offers, null for nothing
void        media_skip();
bool        media_is_video();
extern int  s_media_volume;  // percent, as last reported or set, -1 before either

// Jellyfin fullscreen: the film, its controls, the desk and the lights.
void build_cinema(lv_obj_t *screen);
void open_cinema();
bool cinema_open();
void apply_cinema_still(const void *pixels);
void build_music(lv_obj_t *screen);
void open_music();
bool music_open();
void apply_music_cover(const void *pixels);  // media::kLargeArtSize square, or null
void refresh_music();  // after what plays changed: its title, cover and times together
/** The time in a fullscreen view's bottom right corner, shown or put away by a
 *  tap there, as the last tap left it in any of them. */
// The time beside a fullscreen view's chip back, faded with its other buttons.
lv_obj_t *add_view_clock(ViewId view, lv_obj_t *root, lv_obj_t *chip);
void      update_view_clocks();
// Stand and Sit, the time and the way back, over a fullscreen view.
struct Chrome {
    lv_obj_t *close;
    lv_obj_t *desk;  // its chips are its children
};
Chrome add_fullscreen_chrome(ViewId view, lv_obj_t *root, lv_event_cb_t on_close);
void apply_media_neighbours(bool previous, bool next);
bool cinema_has_next();  // an episode after this one to go on to
void show_guest_presets();
void select_page(int index);
/** The Focus tab's caption in place of its name, and its icon in `ink`, faded
 *  while paused; a null caption puts it back. */
void show_focus_tab(const char *caption, std::uint32_t ink, bool paused);
void brightness_event_cb(lv_event_t *e);
std::uint32_t info_ink(Level level);
void refresh_diag_summary();
void show_diagnostics_cb(lv_event_t *);
void show_appearance_cb(lv_event_t *);
void show_settings_cb(lv_event_t *);
void show_behaviour_cb(lv_event_t *);
void show_log_cb(lv_event_t *);
void apply_splash();

/** For updates kept outside ui.cpp's slots: asks the LVGL task to look. */
void request_apply();
void apply_setting(int index, bool on);
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

}  // namespace ui::detail
