#include "ui_internal.h"

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

constexpr std::int32_t BULB_W           = 34;
constexpr std::int32_t BULB_H           = 48;
constexpr std::int32_t BULB_BASE_W      = 16;
constexpr std::int32_t BULB_BASE_RADIUS = 3;
constexpr std::int32_t BULB_NECK        = 3;  // between the glass and the base
constexpr int          BULB_PARTS       = 2;  // the glass and the base
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
    lv_obj_set_size(base, BULB_BASE_W, BULB_H - BULB_W - BULB_NECK);
    lv_obj_set_pos(base, (BULB_W - BULB_BASE_W) / 2, BULB_W + BULB_NECK);
    theme::style_panel(base, theme::disabled_ink, BULB_BASE_RADIUS);
    bulb_states(base);
    lv_obj_set_clickable(base, false);
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

std::optional<ModalOverlay> s_media_panel;
lv_obj_t *s_panel_frame    = nullptr;
lv_obj_t *s_panel_art      = nullptr;
lv_obj_t *s_panel_title    = nullptr;
lv_obj_t *s_panel_artist   = nullptr;
namespace {
lv_obj_t *s_panel_play     = nullptr;
}  // namespace

lv_obj_t *s_panel_progress = nullptr;
lv_obj_t *s_panel_elapsed  = nullptr;
lv_obj_t *s_panel_total    = nullptr;
namespace {
lv_obj_t *s_panel_quieter  = nullptr;
lv_obj_t *s_panel_louder   = nullptr;
lv_obj_t *s_panel_picks    = nullptr;  // the favourites, from what is playing
}  // namespace

lv_obj_t *s_panel_volume_pct  = nullptr;
namespace {
lv_obj_t *s_panel_volume_icon = nullptr;
}  // namespace

TextBox s_card_with_art{}, s_card_bare{};
TextBox s_panel_with_art{}, s_panel_bare{};
bool    s_has_art = false;
namespace {
constexpr std::int32_t MEDIA_CARD_PAD   = 18;
constexpr std::int32_t CARD_ART_TRIM    = 61;  // how much shorter than the card its cover is
constexpr std::int32_t CARD_ART_RADIUS  = 14;
constexpr std::int32_t CARD_TITLE_Y     = 28;
constexpr std::int32_t TITLE_TO_ARTIST  = 6;

constexpr std::int32_t MEDIA_PANEL_W      = 700;
constexpr std::int32_t MEDIA_PANEL_H      = 408;
constexpr std::int32_t MEDIA_PANEL_PAD    = theme::space::l;
constexpr std::int32_t PANEL_ART          = 200;
constexpr std::int32_t PANEL_ART_RADIUS   = 18;
constexpr std::int32_t ARTIST_TO_PROGRESS = 20;
constexpr std::int32_t PROGRESS_H         = 8;
constexpr std::int32_t PROGRESS_TO_TIMES  = 14;
constexpr std::int32_t TIMES_TO_VOLUME    = 20;
constexpr std::int32_t VOLUME_PCT_X       = 34;
constexpr std::int32_t VOL_W              = 88;
constexpr std::int32_t VOL_H              = 60;
constexpr std::int32_t VOL_GAP            = 12;
constexpr std::int32_t TRANSPORT_H        = 92;
constexpr std::int32_t TRANSPORT_SIDE_W   = 150;
constexpr std::int32_t TRANSPORT_PLAY_W   = 200;

constexpr lv_opa_t PAUSED_ART_DIM  = LV_OPA_50;
constexpr lv_opa_t PLAYING_ART_DIM = LV_OPA_TRANSP;

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

void layout_card_text(const TextBox &card)
{
    const std::int32_t title_h =
        title_height(lv_label_get_text(s_media_title), fonts::size_20(), card.w);
    const std::int32_t artist_y = CARD_TITLE_Y + title_h + TITLE_TO_ARTIST;
    const std::int32_t artist_h = artist_height(fonts::size_16(), s_card_inner_h - artist_y);
    lv_obj_set_height(s_media_artist, artist_h);
    theme::align(s_media_artist, LV_ALIGN_TOP_LEFT, card.x, artist_y);
}

void layout_panel_text(const TextBox &panel)
{
    std::int32_t y =
        title_height(lv_label_get_text(s_panel_title), fonts::size_28(), panel.w) + TITLE_TO_ARTIST;
    const std::int32_t artist_h =
        artist_height(fonts::size_22(), s_panel_inner_h - s_panel_below - y);
    lv_obj_set_height(s_panel_artist, artist_h);
    theme::align(s_panel_artist, LV_ALIGN_TOP_LEFT, panel.x, y);

    y += artist_h + ARTIST_TO_PROGRESS;
    theme::align(s_panel_progress, LV_ALIGN_TOP_LEFT, panel.x, y);

    y += PROGRESS_TO_TIMES;
    theme::align(s_panel_elapsed, LV_ALIGN_TOP_LEFT, panel.x, y);
    theme::align(s_panel_total, LV_ALIGN_TOP_RIGHT, 0, y);

    y += lv_font_get_line_height(fonts::size_16()) + TIMES_TO_VOLUME;
    const std::int32_t text_dy = (VOL_H - lv_font_get_line_height(fonts::size_20())) / 2;
    theme::align(s_panel_volume_icon, LV_ALIGN_TOP_LEFT, panel.x, y + text_dy);
    theme::align(s_panel_volume_pct, LV_ALIGN_TOP_LEFT, panel.x + VOLUME_PCT_X, y + text_dy);
    theme::align(s_panel_picks, LV_ALIGN_TOP_RIGHT, -2 * (VOL_W + VOL_GAP), y);
    theme::align(s_panel_quieter, LV_ALIGN_TOP_RIGHT, -(VOL_W + VOL_GAP), y);
    theme::align(s_panel_louder, LV_ALIGN_TOP_RIGHT, 0, y);
}
}  // namespace

void layout_media_text()
{
    layout_card_text(s_has_art ? s_card_with_art : s_card_bare);
    layout_panel_text(s_has_art ? s_panel_with_art : s_panel_bare);
}

int        s_position_s   = 0;
int        s_duration_s   = 0;
bool       s_media_playing = false;
TickType_t s_position_at  = 0;
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

    lv_obj_t *frame = rounded_frame(view.root, side, PICK_ART_RADIUS);
    lv_obj_align(frame, LV_ALIGN_TOP_LEFT, 0, 0);
    view.art = make_cover(frame, side);
    lv_obj_set_hidden(view.art, true);

    view.name = theme::make_label(view.root, "", theme::text, fonts::size_20());
    one_line(view.name, fonts::size_20(), side);
    lv_obj_align(view.name, LV_ALIGN_TOP_LEFT, 0, side + PICK_NAME_GAP);
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
namespace {
constexpr int PROGRESS_TICK_MS = 200;

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
}  // namespace

lv_timer_t *s_pause_timer     = nullptr;
bool        s_playing_shown   = false;
bool        s_has_track_shown = false;

void apply_playing(bool playing)
{
    s_playing_shown = playing;
    theme::set_text(lv_obj_get_child(s_panel_play, 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    const lv_opa_t dim = s_has_track_shown && !playing ? PAUSED_ART_DIM : PLAYING_ART_DIM;
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
namespace {
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

void media_swiped(lv_dir_t direction)
{
    if (direction != LV_DIR_LEFT && direction != LV_DIR_RIGHT) {
        return;
    }
    s_media_swiped = true;
    if (s_handlers.media != nullptr) {
        s_handlers.media(direction == LV_DIR_LEFT ? MediaAction::Next : MediaAction::Previous);
    }
}

void media_held()
{
    s_media_long = true;
    if (!s_has_track_shown) {
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
    } else if (s_media_panel.has_value()) {
        lv_obj_set_hidden(s_panel_picks, pick_count() == 0);
        s_media_panel->open(s_media_card);
    }
}

void media_tapped()
{
    if (std::exchange(s_media_long, false) || std::exchange(s_media_swiped, false)) {
        return;
    }
    if (!s_has_track_shown) {
        return;  // nothing to pause or carry on with
    }
    cancel_pause_settle();
    apply_playing(!s_playing_shown);
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

lv_obj_t *media_button(lv_obj_t *parent, const char *symbol, MediaAction action, std::int32_t w,
                       std::int32_t h)
{
    lv_obj_t *button = theme::make_button(parent, symbol, theme::panel_light, fonts::size_32());
    lv_obj_set_size(button, w, h);
    lv_obj_add_event_cb(button, media_action_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(action)));
    return button;
}

void build_panel_track(lv_obj_t *card, std::int32_t text_x, std::int32_t text_w)
{
    s_panel_frame = rounded_frame(card, PANEL_ART, PANEL_ART_RADIUS);
    lv_obj_align(s_panel_frame, LV_ALIGN_TOP_LEFT, 0, 0);
    s_panel_art = make_cover(s_panel_frame, PANEL_ART);

    s_panel_title = theme::make_label(card, "--", theme::text, fonts::size_28());
    two_lines(s_panel_title, fonts::size_28(), text_w);
    lv_obj_align(s_panel_title, LV_ALIGN_TOP_LEFT, text_x, 0);

    s_panel_artist = theme::make_label(card, "", theme::secondary, fonts::size_22());
    one_line(s_panel_artist, fonts::size_22(), text_w);
}

void build_panel_progress(lv_obj_t *card, std::int32_t text_w)
{
    s_panel_progress = lv_bar_create(card);
    lv_obj_set_size(s_panel_progress, text_w, PROGRESS_H);
    theme::style_panel(s_panel_progress, theme::panel_light, PROGRESS_H / 2);
    theme::fill_accent(s_panel_progress, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_panel_progress, PROGRESS_H / 2, LV_PART_INDICATOR);

    s_panel_elapsed = theme::make_label(card, "0:00", theme::secondary, fonts::size_16());
    s_panel_total   = theme::make_label(card, "0:00", theme::secondary, fonts::size_16());
}

void panel_picks_cb(lv_event_t *)
{
    s_media_panel->close();
    open_pick_picker();
}

void build_panel_volume(lv_obj_t *card)
{
    s_panel_volume_icon =
        theme::make_label(card, LV_SYMBOL_VOLUME_MAX, theme::secondary, fonts::size_20());
    s_panel_volume_pct = theme::make_label(card, "--", theme::text, fonts::size_20());

    s_panel_picks = theme::make_button(card, LV_SYMBOL_LIST, theme::panel_light, fonts::size_32());
    lv_obj_set_size(s_panel_picks, VOL_W, VOL_H);
    lv_obj_add_event_cb(s_panel_picks, panel_picks_cb, LV_EVENT_CLICKED, nullptr);

    s_panel_quieter = media_button(card, LV_SYMBOL_MINUS, MediaAction::VolumeDown, VOL_W, VOL_H);
    s_panel_louder  = media_button(card, LV_SYMBOL_PLUS, MediaAction::VolumeUp, VOL_W, VOL_H);
}

void build_transport(lv_obj_t *card, std::int32_t inner_w, std::int32_t inner_h)
{
    const std::int32_t row_w = 2 * TRANSPORT_SIDE_W + TRANSPORT_PLAY_W + 2 * BUTTON_GAP;
    const std::int32_t row_x = (inner_w - row_w) / 2;
    const std::int32_t row_y = inner_h - TRANSPORT_H;

    lv_obj_t *previous =
        media_button(card, LV_SYMBOL_PREV, MediaAction::Previous, TRANSPORT_SIDE_W, TRANSPORT_H);
    lv_obj_align(previous, LV_ALIGN_TOP_LEFT, row_x, row_y);

    s_panel_play =
        media_button(card, LV_SYMBOL_PLAY, MediaAction::PlayPause, TRANSPORT_PLAY_W, TRANSPORT_H);
    lv_obj_align(s_panel_play, LV_ALIGN_TOP_LEFT, row_x + TRANSPORT_SIDE_W + BUTTON_GAP, row_y);
    theme::fill_accent(s_panel_play);

    lv_obj_t *next =
        media_button(card, LV_SYMBOL_NEXT, MediaAction::Next, TRANSPORT_SIDE_W, TRANSPORT_H);
    lv_obj_align(next, LV_ALIGN_TOP_LEFT,
                 row_x + TRANSPORT_SIDE_W + TRANSPORT_PLAY_W + 2 * BUTTON_GAP, row_y);
}

void build_media_panel(lv_obj_t *parent)
{
    s_media_panel.emplace(parent, MEDIA_PANEL_W, MEDIA_PANEL_H);
    lv_obj_t *card = s_media_panel->content();
    lv_obj_set_style_pad_all(card, MEDIA_PANEL_PAD, 0);

    const std::int32_t inner_w = MEDIA_PANEL_W - 2 * MEDIA_PANEL_PAD;
    const std::int32_t inner_h = MEDIA_PANEL_H - 2 * MEDIA_PANEL_PAD;
    s_panel_inner_h            = inner_h;
    // The bar sits within PROGRESS_TO_TIMES, which is measured from its top.
    s_panel_below = ARTIST_TO_PROGRESS + PROGRESS_TO_TIMES +
                    lv_font_get_line_height(fonts::size_16()) + TIMES_TO_VOLUME + VOL_H +
                    BUTTON_GAP + TRANSPORT_H;

    s_panel_with_art = {PANEL_ART + MEDIA_PANEL_PAD, inner_w - PANEL_ART - MEDIA_PANEL_PAD};
    s_panel_bare     = {0, inner_w};

    build_panel_track(card, s_panel_with_art.x, s_panel_with_art.w);
    build_panel_progress(card, s_panel_with_art.w);
    build_panel_volume(card);
    build_transport(card, inner_w, inner_h);

    lv_timer_create(progress_tick, PROGRESS_TICK_MS, nullptr);

    s_media_panel->add_close_button();
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

}

constexpr int LIGHTS_SHARE_NUM = 5;
constexpr int LIGHTS_SHARE_DEN = 9;
}  // namespace

void show_speaker_face(bool shown)
{
    lv_obj_set_hidden(s_speaker_face, !shown);
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
    lv_obj_set_hidden(view.art, pixels == nullptr);
    if (pixels == nullptr) {
        return;
    }
    const std::uint32_t bytes = lv_color_format_get_size(LV_COLOR_FORMAT_RGB565);
    view.dsc.header.magic     = LV_IMAGE_HEADER_MAGIC;
    view.dsc.header.cf        = LV_COLOR_FORMAT_RGB565;
    view.dsc.header.w         = media::kPickArtSize;
    view.dsc.header.h         = media::kPickArtSize;
    view.dsc.header.stride    = media::kPickArtSize * bytes;
    view.dsc.data_size        = media::kPickArtSize * media::kPickArtSize * bytes;
    view.dsc.data             = static_cast<const std::uint8_t *>(pixels);
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
    build_media_panel(page);
    build_pick_picker(page);
}

}  // namespace ui::detail
