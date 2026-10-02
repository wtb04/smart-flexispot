#include "ui_internal.h"

#include "media_model.h"
#include "topics.h"

#include "esp_heap_caps.h"

namespace ui::detail {
namespace {
constexpr float DEFAULT_MIN_C     = 15.0f;
constexpr float DEFAULT_MAX_C     = 30.0f;
constexpr int   TENTHS_PER_DEGREE = 10;

constexpr std::int32_t DIAL_CARD_W   = 480;
constexpr std::int32_t DIAL_INSET    = 100;
constexpr std::int32_t DIAL_CHIP     = theme::chip::size;
constexpr std::int32_t DIAL_CHIP_GAP = 10;
constexpr std::int32_t DIAL_ARC_W    = 24;
constexpr std::int32_t DIAL_KNOB_PAD = 10;
// Three quarters of a turn, open at the bottom.
constexpr std::int32_t DIAL_START_ANGLE = 135;
constexpr std::int32_t DIAL_END_ANGLE   = 45;

// From the middle of the ring.
constexpr std::int32_t EYEBROW_TO_VALUE = 22;
constexpr std::int32_t CURRENT_LABEL_DY = -80;
constexpr std::int32_t CURRENT_VALUE_DY = CURRENT_LABEL_DY + EYEBROW_TO_VALUE;
constexpr std::int32_t TARGET_LABEL_DY  = 24;
constexpr std::int32_t TARGET_VALUE_DY  = TARGET_LABEL_DY + EYEBROW_TO_VALUE;

constexpr int          MODE_W_PERCENT = 55;  // of the ring
constexpr std::int32_t MODE_H         = 68;
constexpr std::int32_t MODE_DROP      = 4;   // below the foot of the ring
}  // namespace

lv_obj_t *s_dial         = nullptr;
lv_obj_t *s_dial_current = nullptr;
lv_obj_t *s_dial_target  = nullptr;
lv_obj_t *s_dial_mode    = nullptr;
lv_obj_t *s_dial_toggles[kDialToggleCount] = {};

float s_dial_step = DEFAULT_STEP_C;
bool s_dial_dragging = false;
namespace {
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
}  // namespace

void write_temperature(lv_obj_t *label, float celsius, bool with_unit)
{
    if (celsius < 0.0f) {
        theme::set_text(label, "--");
        return;
    }
    char      text[24];
    const int tenths = static_cast<int>(celsius * TENTHS_PER_DEGREE + 0.5f);
    std::snprintf(text, sizeof(text), with_unit ? "%d.%d °C" : "%d.%d", tenths / TENTHS_PER_DEGREE,
                  tenths % TENTHS_PER_DEGREE);
    theme::set_text(label, text);
}
namespace {
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

// Screwed down in every corner the chips leave free, like the radar's scope.
void add_dial_screws(lv_obj_t *card, std::int32_t inner_w, std::int32_t inner_h)
{
    lv_obj_set_pos(theme::make_screw(card, DIAL_CHIP), 0, 0);
    lv_obj_set_pos(theme::make_screw(card, DIAL_CHIP), 0, inner_h - DIAL_CHIP);
    lv_obj_set_pos(theme::make_screw(card, DIAL_CHIP), inner_w - DIAL_CHIP, inner_h - DIAL_CHIP);
}

void build_dial_arc(lv_obj_t *card, std::int32_t ring, std::int32_t ring_y)
{
    s_dial = lv_arc_create(card);
    lv_obj_set_size(s_dial, ring, ring);
    lv_obj_align(s_dial, LV_ALIGN_TOP_MID, 0, ring_y);
    lv_arc_set_bg_angles(s_dial, DIAL_START_ANGLE, DIAL_END_ANGLE);
    lv_arc_set_rotation(s_dial, 0);
    lv_arc_set_range(s_dial, static_cast<int>(DEFAULT_MIN_C * DIAL_SCALE),
                     static_cast<int>(DEFAULT_MAX_C * DIAL_SCALE));

    lv_obj_set_style_arc_width(s_dial, DIAL_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_dial, lv_color_hex(theme::panel), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_dial, DIAL_ARC_W, LV_PART_INDICATOR);
    theme::arc_accent(s_dial, LV_PART_INDICATOR);
    theme::fill_accent(s_dial, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_dial, DIAL_KNOB_PAD, LV_PART_KNOB);

    lv_obj_add_event_cb(s_dial, arc_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_dial, arc_released_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_dial, arc_released_cb, LV_EVENT_PRESS_LOST, nullptr);
}

void add_dial_readings(lv_obj_t *card, std::int32_t centre)
{
    lv_obj_align(theme::make_label(card, "CURRENT", theme::secondary, fonts::size_16()),
                 LV_ALIGN_TOP_MID, 0, centre + CURRENT_LABEL_DY);
    s_dial_current = theme::make_label(card, "--", theme::text, fonts::temp_64());
    lv_obj_align(s_dial_current, LV_ALIGN_TOP_MID, 0, centre + CURRENT_VALUE_DY);
    lv_obj_align(theme::make_label(card, "TARGET", theme::secondary, fonts::size_16()),
                 LV_ALIGN_TOP_MID, 0, centre + TARGET_LABEL_DY);
    s_dial_target = theme::make_accent_label(card, "--", fonts::temp_34());
    lv_obj_align(s_dial_target, LV_ALIGN_TOP_MID, 0, centre + TARGET_VALUE_DY);
}

void add_dial_toggles(lv_obj_t *card, std::int32_t inner_w)
{
    for (int i = 0; i < kDialToggleCount; ++i) {
        lv_obj_t *chip = theme::make_chip(card, "");
        theme::fill_accent(chip, LV_STATE_CHECKED);
        lv_obj_set_pos(chip, inner_w - DIAL_CHIP - i * (DIAL_CHIP + DIAL_CHIP_GAP), 0);
        lv_obj_add_event_cb(chip, dial_toggle_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        s_dial_toggles[i] = chip;
        lv_obj_set_hidden(chip, true);
    }
}

void add_mode_button(lv_obj_t *card, std::int32_t ring, std::int32_t ring_y)
{
    s_dial_mode = theme::make_button(card, "OFF", theme::panel);
    lv_obj_set_size(s_dial_mode, ring * MODE_W_PERCENT / 100, MODE_H);
    theme::fill_accent(s_dial_mode, LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_dial_mode, MODE_H / 2, 0);
    lv_obj_align(s_dial_mode, LV_ALIGN_TOP_MID, 0, ring_y + ring - MODE_H + MODE_DROP);
    lv_obj_add_event_cb(s_dial_mode, mode_clicked_cb, LV_EVENT_CLICKED, nullptr);
}

void build_thermostat(lv_obj_t *parent, std::int32_t y, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, 0, y);
    lv_obj_set_size(card, w, h);
    theme::style_panel(card, theme::panel_light, theme::radius::card);
    lv_obj_set_style_pad_all(card, PANEL_PAD, 0);

    const std::int32_t inner_w = w - 2 * PANEL_PAD;
    const std::int32_t inner_h = h - 2 * PANEL_PAD;
    const std::int32_t ring    = std::min(w - DIAL_INSET, inner_h);
    const std::int32_t ring_y  = (inner_h - ring) / 2;

    add_dial_screws(card, inner_w, inner_h);
    build_dial_arc(card, ring, ring_y);
    add_dial_readings(card, ring_y + ring / 2);
    add_dial_toggles(card, inner_w);
    add_mode_button(card, ring, ring_y);
}

Hvac s_dial_shown = Hvac::Heating;  // what build_thermostat leaves on screen
}  // namespace

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
namespace {
constexpr std::int32_t PILL_H       = 60;
constexpr std::int32_t PILL_PAD     = 22;
constexpr std::int32_t PILL_GAP     = 12;
constexpr std::int32_t PILL_DOT     = 14;
constexpr std::int32_t PILL_DOT_GAP = 10;
}  // namespace

Pill         s_pills[kPillCount];
namespace {
std::int32_t s_pill_row_w = 0;
}  // namespace

void reflow_pills()
{
    int visible = 0;
    for (const Pill &pill : s_pills) {
        visible += pill.shown ? 1 : 0;
    }
    if (visible == 0) {
        return;
    }
    const std::int32_t w    = (s_pill_row_w - (visible - 1) * PILL_GAP) / visible;
    const std::int32_t text = w - 2 * PILL_PAD - PILL_DOT - PILL_DOT_GAP;
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
namespace {
Pill make_pill(lv_obj_t *strip)
{
    lv_obj_t *pill = lv_obj_create(strip);
    lv_obj_set_size(pill, s_pill_row_w / kPillCount, PILL_H);
    theme::style_panel(pill, theme::panel_light, PILL_H / 2);
    lv_obj_set_style_pad_hor(pill, PILL_PAD, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, PILL_DOT_GAP, 0);

    lv_obj_t *dot = lv_obj_create(pill);
    lv_obj_set_size(dot, PILL_DOT, PILL_DOT);
    theme::style_panel(dot, theme::secondary, PILL_DOT / 2);
    lv_obj_set_clickable(dot, false);

    lv_obj_t *column = lv_obj_create(pill);
    lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(column, 1);
    theme::style_panel(column, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(column, false);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(column, -1, 0);

    lv_obj_t *label = theme::make_label(column, "", theme::secondary, fonts::size_16());
    lv_obj_t *value = theme::make_label(column, "", theme::text, fonts::size_22());
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_set_hidden(pill, true);
    return Pill{pill, dot, column, label, value, false};
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
    lv_obj_set_style_pad_column(strip, PILL_GAP, 0);

    for (Pill &pill : s_pills) {
        pill = make_pill(strip);
    }
}

constexpr std::int32_t BULB_W     = 34;  // the size its pictures are drawn at
constexpr std::int32_t BULB_H     = 48;
constexpr int          BULB_PARTS = 2;  // the glass and the base
constexpr std::int32_t BULB_GAP         = 14;

constexpr std::int32_t LIGHTS_PAD = 28;
}  // namespace

lv_obj_t *s_lights_button = nullptr;
lv_obj_t *s_lights_name   = nullptr;
lv_obj_t *s_lights_state  = nullptr;
lv_obj_t *s_bulbs[kLightCount]   = {};
bool      s_light_on[kLightCount] = {};
bool      s_lights_on             = false;
namespace {
// A long press fires LONG_PRESSED and then CLICKED on release, so without this
// one gesture would both open the picker and toggle the lights.
bool s_lights_long = false;

std::optional<ModalOverlay> s_light_picker;
}  // namespace

LightButton s_lights[kLightCount];
namespace {
void bulb_states(lv_obj_t *part)
{
    lv_obj_set_style_image_recolor_opa(part, LV_OPA_COVER, 0);
    lv_obj_set_style_image_recolor(part, lv_color_hex(theme::disabled_ink), 0);
    theme::tint_accent(part, LV_STATE_CHECKED);
    theme::tint_dim_accent(part, LV_STATE_USER_1);
    lv_obj_set_style_image_recolor(part, lv_color_hex(theme::text),
                                   LV_STATE_CHECKED | LV_STATE_USER_1);
}

// The glass and the base are two pictures over the same ground, each coloured
// by the light's state.
lv_obj_t *make_bulb(lv_obj_t *parent)
{
    lv_obj_t *bulb = lv_obj_create(parent);
    lv_obj_set_size(bulb, BULB_W, BULB_H);
    theme::style_panel(bulb, theme::panel, 0);
    lv_obj_set_style_bg_opa(bulb, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(bulb, false);

    for (const lv_image_dsc_t *part : {&icons::bulb_glass_icon, &icons::bulb_base_icon}) {
        lv_obj_t *image = lv_image_create(bulb);
        lv_image_set_src(image, part);
        lv_obj_set_pos(image, 0, 0);
        bulb_states(image);
        lv_obj_set_clickable(image, false);
    }
    return bulb;
}
}  // namespace

void paint_bulbs()
{
    for (int i = 0; i < kLightCount; ++i) {
        if (s_bulbs[i] == nullptr) {
            continue;
        }
        for (int part = 0; part < BULB_PARTS; ++part) {
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
namespace {
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

void build_bulb_strip(lv_obj_t *button)
{
    lv_obj_t *strip = lv_obj_create(button);
    lv_obj_set_size(strip, LV_SIZE_CONTENT, BULB_H);
    theme::style_panel(strip, theme::panel, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(strip, false);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(strip, BULB_GAP, 0);

    for (lv_obj_t *&bulb : s_bulbs) {
        bulb = make_bulb(strip);
        lv_obj_set_hidden(bulb, true);
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
    lv_obj_set_style_pad_all(s_lights_button, LIGHTS_PAD, 0);
    lv_obj_add_event_cb(s_lights_button, lights_event_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_lights_button, lights_event_cb, LV_EVENT_LONG_PRESSED, nullptr);

    s_lights_name = theme::make_label(s_lights_button, "LIGHTS", theme::secondary,
                                      fonts::size_22());
    lv_obj_align(s_lights_name, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lights_state = theme::make_label(s_lights_button, "--", theme::text, fonts::size_48());
    lv_obj_align(s_lights_state, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    build_bulb_strip(s_lights_button);
}

constexpr std::int32_t PICKER_COLUMNS  = 2;
constexpr std::int32_t PICKER_PAD      = theme::space::l;
constexpr std::int32_t PICKER_HEADER_H = 52;
constexpr std::int32_t LIGHT_BTN_W     = 272;
constexpr std::int32_t LIGHT_BTN_H     = 170;
constexpr std::int32_t LIGHT_BTN_PAD   = 20;

LightButton make_light_button(lv_obj_t *grid, int index)
{
    lv_obj_t *btn = lv_button_create(grid);
    lv_obj_set_size(btn, LIGHT_BTN_W, LIGHT_BTN_H);
    theme::style_button(btn, theme::panel_light);
    theme::fill_accent(btn, LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(btn, LIGHT_BTN_PAD, 0);
    lv_obj_add_event_cb(btn, light_clicked_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    lv_obj_t *name = theme::make_label(btn, "", theme::secondary, fonts::size_20());
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(name, LIGHT_BTN_W - 2 * LIGHT_BTN_PAD);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *state = theme::make_label(btn, "--", theme::text, fonts::size_32());
    lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_set_hidden(btn, true);
    return LightButton{btn, name, state};
}

void build_light_picker(lv_obj_t *parent)
{
    const std::int32_t rows   = (kLightCount + PICKER_COLUMNS - 1) / PICKER_COLUMNS;
    const std::int32_t card_w =
        PICKER_COLUMNS * LIGHT_BTN_W + (PICKER_COLUMNS - 1) * BUTTON_GAP + 2 * PICKER_PAD;
    const std::int32_t card_h =
        rows * LIGHT_BTN_H + (rows - 1) * BUTTON_GAP + PICKER_HEADER_H + 2 * PICKER_PAD;

    s_light_picker.emplace(parent, card_w, card_h);
    lv_obj_t *card = s_light_picker->content();
    lv_obj_set_style_pad_all(card, PICKER_PAD, 0);

    lv_obj_t *title = theme::make_accent_label(card, "LIGHTS", fonts::size_22());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_pos(grid, 0, PICKER_HEADER_H);
    lv_obj_set_size(grid, card_w - 2 * PICKER_PAD, card_h - 2 * PICKER_PAD - PICKER_HEADER_H);
    theme::style_panel(grid, theme::panel, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, BUTTON_GAP, 0);
    lv_obj_set_style_pad_column(grid, BUTTON_GAP, 0);

    for (int i = 0; i < kLightCount; ++i) {
        s_lights[i] = make_light_button(grid, i);
    }

    s_light_picker->add_close_button();
}
}  // namespace

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
namespace {
bool           s_media_long   = false;
bool           s_media_swiped = false;
}  // namespace

TextBox s_card_with_art{}, s_card_bare{};
bool    s_has_art = false;
namespace {
constexpr std::int32_t MEDIA_CARD_PAD   = 18;
constexpr std::int32_t CARD_ART_TRIM    = 61;  // how much shorter than the card its cover is
constexpr std::int32_t CARD_ART_RADIUS  = 14;
constexpr std::int32_t CARD_TITLE_Y     = 28;
constexpr std::int32_t TITLE_TO_ARTIST  = 6;

constexpr lv_opa_t PAUSED_ART_DIM  = LV_OPA_50;
constexpr lv_opa_t PLAYING_ART_DIM = LV_OPA_TRANSP;

std::int32_t s_card_inner_h  = 0;

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

void layout_card_text(const TextBox &card)
{
    const std::int32_t title_h =
        title_height(lv_label_get_text(s_media_title), fonts::size_20(), card.w);
    const std::int32_t artist_y = CARD_TITLE_Y + title_h + TITLE_TO_ARTIST;
    const std::int32_t artist_h = artist_height(fonts::size_16(), s_card_inner_h - artist_y);
    lv_obj_set_height(s_media_artist, artist_h);
    theme::align(s_media_artist, LV_ALIGN_TOP_LEFT, card.x, artist_y);
}

}  // namespace

void layout_media_text()
{
    layout_card_text(s_has_art ? s_card_with_art : s_card_bare);
}

namespace {
constexpr int SECONDS_PER_MINUTE = 60;

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

lv_obj_t *make_cover(lv_obj_t *frame, std::int32_t side)
{
    lv_obj_t *art = lv_image_create(frame);
    lv_obj_set_size(art, side, side);
    lv_obj_center(art);
    lv_image_set_inner_align(art, LV_IMAGE_ALIGN_STRETCH);
    return art;
}
}  // namespace

namespace {
// Favourites, from holding the card while nothing plays: a cover each with its
// name, as many columns as there are favourites up to a row's worth.
constexpr std::int32_t PICK_COLUMNS  = 4;
constexpr std::int32_t PICK_NAME_GAP   = 8;
constexpr std::int32_t PICK_ART_RADIUS = 14;
constexpr std::int32_t PICK_MIN_W      = 320;  // room for the title and the close button

struct PickView {
    lv_obj_t      *root;
    lv_obj_t      *art;
    lv_obj_t      *name;
    lv_image_dsc_t dsc;
    std::uint16_t *rounded;  // the cover with its corners already in the card's colour
    bool           named;
};
PickView                    s_pick_views[media::kPickCount]{};
std::optional<ModalOverlay> s_pick_picker;

std::int32_t pick_height()
{
    return media::kPickArtSize + PICK_NAME_GAP + lv_font_get_line_height(fonts::size_20());
}

void pick_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_pick_picker->close();
    if (s_handlers.pick != nullptr) {
        s_handlers.pick(index);
    }
}

void build_pick(lv_obj_t *grid, int index)
{
    const std::int32_t side = media::kPickArtSize;
    PickView          &view = s_pick_views[index];

    view.root = lv_obj_create(grid);
    lv_obj_set_size(view.root, side, pick_height());
    lv_obj_set_style_bg_opa(view.root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view.root, 0, 0);
    lv_obj_set_style_pad_all(view.root, 0, 0);
    lv_obj_set_scrollable(view.root, false);
    lv_obj_set_style_opa(view.root, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(view.root, pick_clicked_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_set_hidden(view.root, true);

    view.art = lv_image_create(view.root);
    lv_obj_align(view.art, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_clickable(view.art, false);
    lv_obj_set_hidden(view.art, true);

    view.name = theme::make_label(view.root, "", theme::text, fonts::size_20());
    one_line(view.name, fonts::size_20(), side);
    lv_obj_align(view.name, LV_ALIGN_TOP_LEFT, 0, side + PICK_NAME_GAP);
}

std::uint16_t rgb565_of(std::uint32_t colour)
{
    return static_cast<std::uint16_t>(((colour >> 8) & 0xf800) | ((colour >> 5) & 0x07e0) |
                                      ((colour >> 3) & 0x001f));
}

std::uint16_t blend565(std::uint16_t a, std::uint16_t b, int b_share, int whole)
{
    const auto channel = [&](int shift, int mask) {
        const int ca = (a >> shift) & mask;
        const int cb = (b >> shift) & mask;
        return ((ca * (whole - b_share) + cb * b_share) / whole) << shift;
    };
    constexpr int RED = 11, GREEN = 5, FIVE_BITS = 0x1f, SIX_BITS = 0x3f;
    return static_cast<std::uint16_t>(channel(RED, FIVE_BITS) | channel(GREEN, SIX_BITS) |
                                      channel(0, FIVE_BITS));
}

/** A square picture copied with its corners rounded over `background`, their
 *  edge smoothed by sampling each corner pixel several times. */
void round_corners(const std::uint16_t *from, std::uint16_t *to, std::int32_t side,
                   std::int32_t radius, std::uint32_t background)
{
    constexpr int SAMPLES = 4;  // a side, per pixel
    constexpr int WHOLE   = SAMPLES * SAMPLES;
    const std::uint16_t fill = rgb565_of(background);
    std::copy(from, from + side * side, to);
    for (std::int32_t y = 0; y < radius; ++y) {
        for (std::int32_t x = 0; x < radius; ++x) {
            int outside = 0;
            for (int sy = 0; sy < SAMPLES; ++sy) {
                for (int sx = 0; sx < SAMPLES; ++sx) {
                    const float dx = static_cast<float>(radius) - (x + (sx + 0.5f) / SAMPLES);
                    const float dy = static_cast<float>(radius) - (y + (sy + 0.5f) / SAMPLES);
                    outside += dx * dx + dy * dy > static_cast<float>(radius * radius) ? 1 : 0;
                }
            }
            if (outside == 0) {
                continue;
            }
            // The same share at each of the four corners, mirrored.
            for (const auto &[px, py] : {std::pair{x, y}, std::pair{side - 1 - x, y},
                                        std::pair{x, side - 1 - y},
                                        std::pair{side - 1 - x, side - 1 - y}}) {
                std::uint16_t &pixel = to[py * side + px];
                pixel                = blend565(pixel, fill, outside, WHOLE);
            }
        }
    }
}

/** Sized for the favourites there are, since that changes between openings. */
void fit_pick_picker(int count)
{
    const std::int32_t columns = std::min<std::int32_t>(count, PICK_COLUMNS);
    const std::int32_t rows    = (count + PICK_COLUMNS - 1) / PICK_COLUMNS;
    const std::int32_t grid_w  = columns * media::kPickArtSize + (columns - 1) * BUTTON_GAP;
    const std::int32_t grid_h  = rows * pick_height() + (rows - 1) * BUTTON_GAP;
    s_pick_picker->resize(std::max(grid_w, PICK_MIN_W) + 2 * PICKER_PAD,
                          grid_h + PICKER_HEADER_H + 2 * PICKER_PAD);
}

void build_pick_picker(lv_obj_t *parent)
{
    s_pick_picker.emplace(parent, PICK_MIN_W, PICKER_HEADER_H);
    lv_obj_t *card = s_pick_picker->content();
    lv_obj_set_style_pad_all(card, PICKER_PAD, 0);

    lv_obj_t *title = theme::make_accent_label(card, "FAVOURITES", fonts::size_22());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_pos(grid, 0, PICKER_HEADER_H);
    lv_obj_set_size(grid, PICK_COLUMNS * (media::kPickArtSize + BUTTON_GAP), LV_SIZE_CONTENT);
    theme::style_panel(grid, theme::panel, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(grid, false);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, BUTTON_GAP, 0);
    lv_obj_set_style_pad_column(grid, BUTTON_GAP, 0);

    for (int i = 0; i < media::kPickCount; ++i) {
        build_pick(grid, i);
    }
    s_pick_picker->add_close_button();
}

int pick_count()
{
    int count = 0;
    for (const PickView &view : s_pick_views) {
        count += view.named ? 1 : 0;
    }
    return count;
}

void open_pick_picker()
{
    const int count = pick_count();
    if (count == 0 || !s_pick_picker.has_value()) {
        return;
    }
    fit_pick_picker(count);
    s_pick_picker->open(s_media_card);
}
}  // namespace


void write_clock(lv_obj_t *label, int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", seconds / SECONDS_PER_MINUTE,
                  seconds % SECONDS_PER_MINUTE);
    theme::set_text(label, text);
}

lv_timer_t *s_pause_timer     = nullptr;

void apply_playing(bool playing)
{
    media_state().playing = playing;
    publish(Topic::Media);

    const lv_opa_t dim = media_state().has_track && !playing ? PAUSED_ART_DIM : PLAYING_ART_DIM;
    if (lv_obj_get_style_image_recolor_opa(s_media_art, LV_PART_MAIN) != dim) {
        lv_obj_set_style_image_recolor(s_media_art, lv_color_hex(theme::background), 0);
        lv_obj_set_style_image_recolor_opa(s_media_art, dim, 0);
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
namespace {

// A player that takes no commands is followed, not steered: whatever was
// tapped, nothing that would play, pause, skip or seek it is sent.
bool steers(MediaAction action)
{
    return media_state().remote || action == MediaAction::VolumeDown || action == MediaAction::VolumeUp ||
           action == MediaAction::Mute;
}

// A video's intro and credits, and the button that skips them: into the intro
// it skips to the intro's end, into the credits on to the next episode, which
// Jellyfin starts once this one runs out.
constexpr int           SEEK_STEP_S    = 10;
constexpr int           NEXT_UP_TAIL_S = 30;  // with no credits marked, the end is near
constexpr std::uint32_t SKIP_CHECK_MS  = 500;
constexpr std::int32_t  SKIP_H         = 40;
constexpr std::int32_t  SKIP_PAD       = 16;
constexpr std::int32_t  EXPAND_CHIP    = theme::chip::size;

MediaSegment s_segments[kMaxSegments]{};
int          s_segment_count = 0;
lv_obj_t    *s_skip          = nullptr;
lv_obj_t    *s_expand        = nullptr;  // into the cinema view for a video, the music view else
int          s_skip_to       = -1;    // where skipping the intro seeks to
bool         s_skip_next     = false; // skipping starts the next episode instead
const char  *s_skip_text     = nullptr;

int position_now()
{
    return media_position_now();
}

void seek_to(int position_s)
{
    if (!media_state().remote) {
        return;
    }
    position_s    = std::max(0, media_state().duration_s > 0 ? std::min(position_s, media_state().duration_s) : position_s);
    media_state().position_s  = position_s;
    media_state().position_at = xTaskGetTickCount();
    publish(Topic::Media);
    if (s_handlers.seek != nullptr) {
        s_handlers.seek(position_s);
    }
}

void skip_check(lv_timer_t *)
{
    const char *text = nullptr;
    int         to   = -1;
    bool        next = false;
    if (media_state().video && media_state().has_track && media_state().duration_s > 0) {
        const int at            = position_now();
        bool      credits_known = false;
        for (int i = 0; i < s_segment_count; ++i) {
            const MediaSegment &segment = s_segments[i];
            const bool          intro   = segment.kind == MediaSegment::Kind::Intro;
            credits_known               = credits_known || !intro;
            if (at >= segment.start_s && at < segment.end_s) {
                if (intro) {
                    text = "Skip intro";
                    to   = segment.end_s;
                } else {
                    next = true;
                }
            }
        }
        next = next || (!credits_known && media_state().duration_s > NEXT_UP_TAIL_S &&
                        at >= media_state().duration_s - NEXT_UP_TAIL_S);
        // Seeking to the end only stops the player there; the episode after
        // is started instead, and without one there is nothing to go on to.
        next = next && cinema_has_next();
        if (next) {
            text = "Next episode";
        }
    }
    s_skip_to   = to;
    s_skip_next = next;
    s_skip_text = text;
    lv_obj_set_hidden(s_skip, text == nullptr);
    lv_obj_set_hidden(s_expand, !media_state().has_track);
    if (text != nullptr) {
        theme::set_text(lv_obj_get_child(s_skip, 0), text);
    }
}

void skip_clicked_cb(lv_event_t *)
{
    if (s_skip_to >= 0 || s_skip_next) {
        media_skip();
        lv_obj_set_hidden(s_skip, true);
    }
}

void build_skip_button(lv_obj_t *card)
{
    s_skip = theme::make_button(card, "Skip intro", theme::panel_light, theme::type_body());
    theme::fill_accent(s_skip);
    lv_obj_set_size(s_skip, LV_SIZE_CONTENT, SKIP_H);
    lv_obj_set_style_pad_hor(s_skip, SKIP_PAD, 0);
    lv_obj_align(s_skip, LV_ALIGN_BOTTOM_RIGHT, -(EXPAND_CHIP + BUTTON_GAP), 0);
    lv_obj_add_event_cb(s_skip, skip_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_skip, true);
    lv_timer_create(skip_check, SKIP_CHECK_MS, nullptr);

    // A corner chip, as the radar's and the focus dial's.
    s_expand = theme::make_chip(card, "");
    theme::make_mark(s_expand, &icons::expand_icon);
    lv_obj_align(s_expand, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(
        s_expand, [](lv_event_t *) { media_is_video() ? open_cinema() : open_music(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_expand, true);
}

void media_swiped(lv_dir_t direction)
{
    s_media_swiped = true;
    // Up louder, down quieter, whatever plays.
    if (direction == LV_DIR_TOP || direction == LV_DIR_BOTTOM) {
        if (s_handlers.media != nullptr) {
            s_handlers.media(direction == LV_DIR_TOP ? MediaAction::VolumeUp
                                                     : MediaAction::VolumeDown);
        }
        return;
    }
    if (!media_state().remote) {
        return;
    }
    // A video goes ten seconds on or back, as its own player does.
    if (media_state().video) {
        seek_to(position_now() + (direction == LV_DIR_LEFT ? SEEK_STEP_S : -SEEK_STEP_S));
        return;
    }
    if (s_handlers.media != nullptr) {
        s_handlers.media(direction == LV_DIR_LEFT ? MediaAction::Next : MediaAction::Previous);
    }
}

void media_held()
{
    s_media_long = true;
    if (!media_state().has_track) {
        open_pick_picker();
        return;
    }
    if (s_media_off) {
        return;
    }
    if (s_media_hold >= 0) {
        if (s_handlers.preset != nullptr) {
            s_handlers.preset(s_media_hold, false);
        }
    } else {
        open_music();
    }
}

void media_tapped()
{
    if (std::exchange(s_media_long, false) || std::exchange(s_media_swiped, false)) {
        return;
    }
    if (!media_state().has_track || !media_state().remote) {
        return;  // nothing to pause or carry on with, or a player that will not
    }
    cancel_pause_settle();
    apply_playing(!media_state().playing);
    if (s_handlers.media != nullptr) {
        s_handlers.media(MediaAction::PlayPause);
    }
}

void media_card_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_GESTURE) {
        media_swiped(lv_indev_get_gesture_dir(lv_indev_active()));
    } else if (code == LV_EVENT_LONG_PRESSED) {
        media_held();
    } else {
        media_tapped();
    }
}

// While nothing plays the cover's frame shows the speaker itself.
lv_obj_t *s_speaker_face = nullptr;

void build_speaker_face(lv_obj_t *frame, std::int32_t side)
{
    s_speaker_face = lv_image_create(frame);
    lv_image_set_src(s_speaker_face, speaker_picture(side));
    lv_obj_center(s_speaker_face);
    lv_obj_set_clickable(s_speaker_face, false);
    lv_obj_set_hidden(s_speaker_face, true);
}

void build_media_card(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                      std::int32_t h)
{
    s_media_card = lv_button_create(parent);
    lv_obj_set_pos(s_media_card, x, y);
    lv_obj_set_size(s_media_card, w, h);
    theme::style_button(s_media_card, theme::panel_light);
    lv_obj_set_style_pad_all(s_media_card, MEDIA_CARD_PAD, 0);
    lv_obj_add_event_cb(s_media_card, media_card_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(s_media_card, media_card_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(s_media_card, media_card_cb, LV_EVENT_GESTURE, nullptr);
    // Gestures bubble by default, so LVGL walks past the card to the screen and a
    // handler here never runs.
    lv_obj_set_gesture_bubble(s_media_card, false);

    const std::int32_t art = h - CARD_ART_TRIM;
    s_media_frame          = rounded_frame(s_media_card, art, CARD_ART_RADIUS);
    lv_obj_align(s_media_frame, LV_ALIGN_LEFT_MID, 0, 0);
    s_media_art = make_cover(s_media_frame, art);
    build_speaker_face(s_media_frame, art);
    lv_obj_set_hidden(s_media_frame, true);

    const std::int32_t inner_w = w - 2 * MEDIA_CARD_PAD;
    s_card_inner_h             = h - 2 * MEDIA_CARD_PAD;
    s_card_with_art            = {art + MEDIA_CARD_PAD, inner_w - art - MEDIA_CARD_PAD};
    s_card_bare                = {0, inner_w};
    const std::int32_t text_x  = s_card_bare.x;
    const std::int32_t text_w  = s_card_bare.w;

    s_media_source = theme::make_label(s_media_card, "SPEAKER", theme::secondary, fonts::size_16());
    lv_obj_align(s_media_source, LV_ALIGN_TOP_LEFT, text_x, 0);

    s_media_title = theme::make_label(s_media_card, "--", theme::text, fonts::size_20());
    two_lines(s_media_title, fonts::size_20(), text_w);
    lv_obj_align(s_media_title, LV_ALIGN_TOP_LEFT, text_x, CARD_TITLE_Y);

    s_media_artist = theme::make_label(s_media_card, "", theme::secondary, fonts::size_16());
    one_line(s_media_artist, fonts::size_16(), text_w);
    build_skip_button(s_media_card);
}

constexpr int LIGHTS_SHARE_NUM = 5;
constexpr int LIGHTS_SHARE_DEN = 9;
}  // namespace

std::int32_t idle_media_text_top()
{
    const std::int32_t block = CARD_TITLE_Y + lv_font_get_line_height(fonts::size_20());
    return std::max<std::int32_t>(0, (s_card_inner_h - block) / 2);
}

void show_speaker_face(bool shown)
{
    lv_obj_set_hidden(s_speaker_face, !shown);
}


void open_favourites()
{
    open_pick_picker();
}

void media_seek_by(int delta_s)
{
    seek_to(position_now() + delta_s);
}

void media_toggle_play()
{
    if (!steers(MediaAction::PlayPause)) {
        return;
    }
    cancel_pause_settle();
    apply_playing(!media_state().playing);
    if (s_handlers.media != nullptr) {
        s_handlers.media(MediaAction::PlayPause);
    }
}

const char *media_skip_text()
{
    return s_skip_text;
}

void media_skip()
{
    if (!media_state().remote) {
        return;
    }
    if (s_skip_next) {
        if (s_handlers.media != nullptr) {
            s_handlers.media(MediaAction::Next);
        }
    } else if (s_skip_to >= 0) {
        seek_to(s_skip_to);
    }
}

void apply_media_segments(const MediaSegment *segments, int count)
{
    s_segment_count = count;
    std::copy(segments, segments + count, s_segments);
}

void apply_media_seeks(bool seeks)
{
    media_state().video = seeks;
    publish(Topic::Media);
}

void apply_media_remote(bool remote)
{
    media_state().remote = remote;
    publish(Topic::Media);
}

bool media_remote()
{
    return media_state().remote;
}

void apply_pick(int index, const char *name)
{
    PickView &view = s_pick_views[index];
    if (view.root == nullptr) {
        return;
    }
    view.named = name != nullptr && name[0] != '\0';
    theme::set_text(view.name, view.named ? name : "");
    lv_obj_set_hidden(view.root, !view.named);
}

void apply_pick_art(int index, const void *pixels)
{
    PickView &view = s_pick_views[index];
    if (view.art == nullptr) {
        return;
    }
    if (pixels != nullptr && view.rounded == nullptr) {
        view.rounded = static_cast<std::uint16_t *>(heap_caps_malloc(
            media::kPickArtSize * media::kPickArtSize * sizeof(std::uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    lv_obj_set_hidden(view.art, pixels == nullptr || view.rounded == nullptr);
    if (pixels == nullptr || view.rounded == nullptr) {
        return;
    }
    // Rounded once here rather than clipped on every frame, which in software
    // costs more than the rest of the popup.
    round_corners(static_cast<const std::uint16_t *>(pixels), view.rounded, media::kPickArtSize,
                  PICK_ART_RADIUS, theme::panel);
    const std::uint32_t bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    view.dsc.header.magic     = LV_IMAGE_HEADER_MAGIC;
    view.dsc.header.cf        = LV_COLOR_FORMAT_RGB565;
    view.dsc.header.w         = media::kPickArtSize;
    view.dsc.header.h         = media::kPickArtSize;
    view.dsc.header.stride    = media::kPickArtSize * bytes;
    view.dsc.data_size        = media::kPickArtSize * media::kPickArtSize * bytes;
    view.dsc.data             = reinterpret_cast<const std::uint8_t *>(view.rounded);
    lv_image_set_src(view.art, &view.dsc);
    lv_obj_invalidate(view.art);
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
    const std::int32_t lights_h = body_h * LIGHTS_SHARE_NUM / LIGHTS_SHARE_DEN;
    build_lights_button(page, col_x, body_y, col_w, lights_h);

    const std::int32_t media_y = body_y + lights_h + BUTTON_GAP;
    build_media_card(page, col_x, media_y, col_w, inner_h - media_y);

    build_light_picker(page);
    build_pick_picker(lv_obj_get_screen(page));  // over the music view too, which is opened from there
}

}  // namespace ui::detail
