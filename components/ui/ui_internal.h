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
#include "calendar_page.h"
#include "screenshot.h"
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

namespace ui::detail {
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
    char title[64];
    char message[192];
    char level[12];
    int  timeout_ms;
};

// What the screen calls each preset. Everything off the screen says Preset 1 to 6.
constexpr const char *PRESET_NAMES[kPresetCount] = {
    "Preset 1", "Preset 2", "Stand", "Sit", "Sit 2", "Stand 2",
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

constexpr int CALENDAR_PAGE = 1;

constexpr int INFO_COUNT = static_cast<int>(Info::Count);

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

constexpr int SETTING_COUNT = static_cast<int>(Setting::Count);

constexpr std::int32_t ROW_CARD_H   = 88;
constexpr std::int32_t DIAG_HEADER_H = 56;
constexpr std::int32_t HEADER_GAP   = 22;
constexpr std::int32_t DETAIL_PAD   = 24;

extern bool s_rail_right;
extern bool s_flipped;
extern Handlers s_handlers;
extern bool s_screen_on;
extern bool s_notice_lit_screen;
extern int s_initial_brightness;
extern std::optional<SegmentDisplay> s_height;
extern lv_obj_t *s_rail;
extern lv_obj_t *s_content;
extern lv_obj_t *s_clock_box;
extern lv_obj_t *s_side_buttons[2];
extern lv_obj_t *s_flip_buttons[2];
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
extern std::optional<ModalOverlay> s_media_panel;
extern lv_obj_t *s_panel_frame;
extern lv_obj_t *s_panel_art;
extern lv_obj_t *s_panel_title;
extern lv_obj_t *s_panel_artist;
extern lv_obj_t *s_panel_progress;
extern lv_obj_t *s_panel_elapsed;
extern lv_obj_t *s_panel_total;
extern lv_obj_t *s_panel_volume_pct;
extern TextBox s_card_with_art, s_card_bare;
extern TextBox s_panel_with_art, s_panel_bare;
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
extern lv_obj_t *s_info[INFO_COUNT];
extern lv_obj_t *s_settings_view;
extern lv_obj_t *s_appearance_view;
extern lv_obj_t *s_diag_view;
extern lv_obj_t *s_diag_summary;
extern lv_obj_t *s_volume_value;
extern lv_obj_t *s_volume_slider;
extern lv_obj_t *s_tile_value[INFO_CARD_COUNT];
extern lv_obj_t *s_tile_dot[INFO_CARD_COUNT];
extern Level s_card_level[INFO_CARD_COUNT];
extern int s_summary_card[INFO_COUNT];

Layout layout();
void set_screen_state(bool on);
void screen_off_cb(lv_event_t *);
void register_desk_control(lv_obj_t *obj);
void show_next_notice();
void create_notice_card();
void create_rail(lv_obj_t *parent);
void place_drawer(std::int32_t width);
void create_drawer(lv_obj_t *parent);
void paint_choice(lv_obj_t *const buttons[2], bool second);
void paint_side_buttons();
void apply_rail_side(bool right);
void write_temperature(lv_obj_t *label, float celsius, bool with_unit);
void paint_dial(Hvac state);
void reflow_pills();
std::uint32_t level_ink(Level level);
void paint_bulbs();
void paint_light(lv_obj_t *root, lv_obj_t *name, lv_obj_t *state, bool on);
void layout_media_text();
void write_clock(lv_obj_t *label, int seconds);
void apply_playing(bool playing);
void cancel_pause_settle();
void pause_settled(lv_timer_t *);
void build_home_page(lv_obj_t *page);
void show_guest_presets();
void select_page(int index);
void brightness_event_cb(lv_event_t *e);
std::uint32_t info_ink(Level level);
void refresh_diag_summary();
void show_diagnostics_cb(lv_event_t *);
void show_appearance_cb(lv_event_t *);
void show_settings_cb(lv_event_t *);
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
lv_obj_t *build_slider_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                            const char *title, int value, int low, lv_event_cb_t changed,
                            lv_obj_t **out_value);
void build_settings_page(lv_obj_t *page);
void create_content(lv_obj_t *parent);
void build_splash();

}  // namespace ui::detail
