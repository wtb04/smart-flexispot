#include "ui.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "board.h"
#include "lvgl.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace ui {
namespace {

constexpr char TAG[] = "ui";

constexpr std::uint32_t LOCK_TIMEOUT_MS = 100;

constexpr std::uint32_t COLOR_BG     = 0x101418;
constexpr std::uint32_t COLOR_TITLE  = 0x8a94a0;
constexpr std::uint32_t COLOR_VALUE  = 0x35d07f;
constexpr std::uint32_t COLOR_STATUS = 0x5c6672;
constexpr std::uint32_t COLOR_TEXT   = 0xf0f4f8;

constexpr std::int32_t BUTTON_W   = 200;
constexpr std::int32_t BUTTON_H   = 160;
constexpr std::int32_t BUTTON_GAP = 40;
constexpr std::int32_t PRESET_W   = 96;
constexpr std::int32_t PRESET_H   = 96;

MoveHandler   s_on_move    = nullptr;
PresetHandler s_on_preset  = nullptr;
lv_obj_t   *s_value_label  = nullptr;
lv_obj_t   *s_status_label  = nullptr;
lv_obj_t   *s_battery_label = nullptr;
lv_obj_t   *s_wifi_label    = nullptr;
lv_obj_t   *s_time_label    = nullptr;
BrightnessHandler s_on_brightness   = nullptr;
int               s_initial_brightness = 80;

void move_event_cb(lv_event_t *e)
{
    if (s_on_move == nullptr) {
        return;
    }
    const auto direction = static_cast<Move>(
        reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_on_move(lv_event_get_code(e) == LV_EVENT_PRESSED ? direction : Move::Stop);
}

// Press starts the motion, release stops it. PRESS_LOST matters as much as
// RELEASED: a finger sliding off the button must not leave the desk travelling.
void create_move_button(lv_obj_t *parent, const char *symbol, Move direction)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, BUTTON_W, BUTTON_H);

    auto *user_data = reinterpret_cast<void *>(
        static_cast<std::intptr_t>(std::to_underlying(direction)));
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_RELEASED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESS_LOST, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text_static(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
    lv_obj_center(label);
}

// Tap to travel there, hold to save the current height into it -- the same
// gesture the physical handset uses.
void preset_event_cb(lv_event_t *e)
{
    if (s_on_preset == nullptr) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_on_preset(index, lv_event_get_code(e) == LV_EVENT_LONG_PRESSED);
}

void create_preset_button(lv_obj_t *parent, int index)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, PRESET_W, PRESET_H);

    auto *user_data = reinterpret_cast<void *>(static_cast<std::intptr_t>(index));
    lv_obj_add_event_cb(btn, preset_event_cb, LV_EVENT_SHORT_CLICKED, user_data);
    lv_obj_add_event_cb(btn, preset_event_cb, LV_EVENT_LONG_PRESSED, user_data);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text_fmt(label, "%d", index + 1);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_center(label);
}

lv_obj_t *create_button_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, BUTTON_GAP, 0);
    return row;
}

// --- notifications ---------------------------------------------------------

constexpr int  NOTIFY_QUEUE_LEN  = 4;
constexpr int  NOTIFY_DEFAULT_MS = 8000;
constexpr std::int32_t NOTIFY_W  = 620;
constexpr std::int32_t NOTIFY_H  = 220;

struct Notice {
    char title[64];
    char message[192];
    char level[12];
    int  timeout_ms;
};

Notice   s_notice_queue[NOTIFY_QUEUE_LEN];
int      s_notice_count = 0;
lv_obj_t *s_notice_card  = nullptr;
lv_obj_t *s_notice_bar   = nullptr;
lv_obj_t *s_notice_title = nullptr;
lv_obj_t *s_notice_body  = nullptr;
lv_timer_t *s_notice_timer = nullptr;

std::uint32_t level_color(const char *level)
{
    if (std::strcmp(level, "error") == 0)   return 0xe05252;
    if (std::strcmp(level, "warning") == 0) return 0xe0a52e;
    if (std::strcmp(level, "success") == 0) return 0x35d07f;
    return 0x4a90d9;  // info, and anything unrecognised
}

void hide_notice();

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

    lv_obj_set_style_bg_color(s_notice_bar, lv_color_hex(level_color(notice.level)), 0);
    // A message with no title reads better promoted into the title slot than
    // shown as small print under an empty heading.
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

void hide_notice()
{
    lv_obj_set_hidden(s_notice_card, true);
    if (s_notice_timer != nullptr) {
        lv_timer_pause(s_notice_timer);
    }
}

void notice_timeout_cb(lv_timer_t *)
{
    show_next_notice();
}

void notice_tapped_cb(lv_event_t *)
{
    show_next_notice();
}

// Lives on the top layer rather than the screen: that layer is drawn above
// everything and takes no part in the screen's flex column, so the card cannot
// be pushed around or covered by the rest of the page.
void create_notice_card()
{
    s_notice_card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_notice_card, NOTIFY_W, NOTIFY_H);
    lv_obj_align(s_notice_card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_notice_card, lv_color_hex(0x1b2128), 0);
    lv_obj_set_style_border_width(s_notice_card, 0, 0);
    lv_obj_set_style_radius(s_notice_card, 12, 0);
    lv_obj_set_style_border_width(s_notice_card, 2, 0);
    lv_obj_set_style_border_color(s_notice_card, lv_color_hex(0x39424e), 0);
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
    // Explicit: the card is dark and the inherited theme colour is not, so
    // without this the title renders in near-black on near-black.
    lv_obj_set_style_text_color(s_notice_title, lv_color_hex(COLOR_TEXT), 0);

    s_notice_body = lv_label_create(s_notice_card);
    lv_obj_set_width(s_notice_body, NOTIFY_W - 80);
    lv_label_set_long_mode(s_notice_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_notice_body, LV_ALIGN_TOP_LEFT, 24, 76);
    lv_obj_set_style_text_color(s_notice_body, lv_color_hex(COLOR_TITLE), 0);
    lv_obj_set_style_text_font(s_notice_body, &lv_font_montserrat_28, 0);

    s_notice_timer = lv_timer_create(notice_timeout_cb, NOTIFY_DEFAULT_MS, nullptr);
    lv_timer_pause(s_notice_timer);
}

void brightness_event_cb(lv_event_t *e)
{
    if (s_on_brightness == nullptr) {
        return;
    }
    auto *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    s_on_brightness(static_cast<int>(lv_slider_get_value(slider)));
}

void create_brightness_slider(lv_obj_t *parent, int initial)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 420, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_pad_top(row, 24, 0);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text_static(icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_STATUS), 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_width(slider, 320);
    // Starts at the lowest brightness the panel honours: a slider whose bottom
    // third does nothing reads as broken.
    lv_slider_set_range(slider, board::kMinBrightness, 100);
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

void build_screen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_static(title, "Smart Flexispot");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TITLE), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    s_value_label = lv_label_create(scr);
    lv_label_set_text_static(s_value_label, "--.-");
    lv_obj_set_style_text_color(s_value_label, lv_color_hex(COLOR_VALUE), 0);
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_48, 0);

    s_status_label = lv_label_create(scr);
    lv_label_set_text_static(s_status_label, "");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_STATUS), 0);

    lv_obj_t *row = create_button_row(scr);
    create_move_button(row, LV_SYMBOL_DOWN, Move::Down);
    create_move_button(row, LV_SYMBOL_UP, Move::Up);

    lv_obj_t *presets = create_button_row(scr);
    lv_obj_set_style_pad_top(presets, 24, 0);
    for (int i = 0; i < kPresetCount; ++i) {
        create_preset_button(presets, i);
    }

    // The status bar sits outside the flex flow so it stays pinned to the top.
    s_time_label = lv_label_create(scr);
    lv_label_set_text_static(s_time_label, "--:--");
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(COLOR_STATUS), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_28, 0);
    lv_obj_set_ignore_layout(s_time_label, true);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_LEFT, 24, 24);

    s_wifi_label = lv_label_create(scr);
    lv_label_set_text_static(s_wifi_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(COLOR_STATUS), 0);
    lv_obj_set_ignore_layout(s_wifi_label, true);
    lv_obj_align(s_wifi_label, LV_ALIGN_TOP_RIGHT, -24, 24);

    s_battery_label = lv_label_create(scr);
    lv_label_set_text_static(s_battery_label, "");
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(COLOR_STATUS), 0);
    lv_obj_set_ignore_layout(s_battery_label, true);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -24, 56);

    create_brightness_slider(scr, s_initial_brightness);

    create_notice_card();

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text_static(hint, "tap a preset to go there, hold to save");
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_STATUS), 0);
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
    ESP_RETURN_ON_FALSE(s_value_label != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    if (height_mm < 0) {
        lv_label_set_text_static(s_value_label, "--.-");
    } else {
        lv_label_set_text_fmt(s_value_label, "%d.%d", height_mm / 10, height_mm % 10);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_battery(bool present, int percent, float volts, int milliamps, bool charging)
{
    ESP_RETURN_ON_FALSE(s_battery_label != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    if (!present) {
        lv_label_set_text_static(s_battery_label, LV_SYMBOL_USB " no battery");
    } else {
        // Hundredths by hand: LVGL's formatter has no float support built in.
        const int centivolts = static_cast<int>(volts * 100.0f + 0.5f);
        lv_label_set_text_fmt(s_battery_label, "%s %d%%  %d.%02d V  %d mA",
                              charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL, percent,
                              centivolts / 100, centivolts % 100, milliamps);
    }
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_wifi(bool connected)
{
    ESP_RETURN_ON_FALSE(s_wifi_label != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_label_set_text_static(s_wifi_label, connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(s_wifi_label,
                                lv_color_hex(connected ? COLOR_VALUE : COLOR_STATUS), 0);
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t set_time(const char *text)
{
    ESP_RETURN_ON_FALSE(s_time_label != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_label_set_text(s_time_label, text != nullptr ? text : "--:--");
    lvgl_port_unlock();
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

esp_err_t set_status(const char *text)
{
    ESP_RETURN_ON_FALSE(s_status_label != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_label_set_text(s_status_label, text != nullptr ? text : "");
    lvgl_port_unlock();
    return ESP_OK;
}

}  // namespace ui
