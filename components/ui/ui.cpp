#include "ui.h"

#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <utility>

namespace ui {
namespace {

constexpr char TAG[] = "ui";

constexpr std::uint32_t LOCK_TIMEOUT_MS = 100;

constexpr std::uint32_t COLOR_BG     = 0x101418;
constexpr std::uint32_t COLOR_TITLE  = 0x8a94a0;
constexpr std::uint32_t COLOR_VALUE  = 0x35d07f;
constexpr std::uint32_t COLOR_STATUS = 0x5c6672;

constexpr std::int32_t BUTTON_W   = 200;
constexpr std::int32_t BUTTON_H   = 160;
constexpr std::int32_t BUTTON_GAP = 40;
constexpr std::int32_t PRESET_W   = 96;
constexpr std::int32_t PRESET_H   = 96;

MoveHandler   s_on_move    = nullptr;
PresetHandler s_on_preset  = nullptr;
lv_obj_t   *s_value_label  = nullptr;
lv_obj_t   *s_status_label = nullptr;

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

void build_screen()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_static(title, "Desk");
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

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text_static(hint, "tap a preset to go there, hold to save");
    lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_STATUS), 0);
}

}  // namespace

esp_err_t init(MoveHandler on_move, PresetHandler on_preset)
{
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    s_on_move   = on_move;
    s_on_preset = on_preset;
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

esp_err_t set_status(const char *text)
{
    ESP_RETURN_ON_FALSE(s_status_label != nullptr, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    ESP_RETURN_ON_FALSE(lvgl_port_lock(LOCK_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "lvgl lock");
    lv_label_set_text(s_status_label, text != nullptr ? text : "");
    lvgl_port_unlock();
    return ESP_OK;
}

}  // namespace ui
