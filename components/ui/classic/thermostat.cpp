#include "ui_internal.h"

#include "home_model.h"
#include "topics.h"

// The thermostat: a dial to drag round to the target, the room's and the
// target's readings in it, its mode under them, and any toggles in its corners,
// as the control bar's heating card.
namespace ui::detail {
namespace {
constexpr float DEFAULT_MIN_C     = 15.0f;
constexpr float DEFAULT_MAX_C     = 30.0f;
constexpr int   TENTHS_PER_DEGREE = 10;

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
constexpr int   DIAL_SCALE     = 10;  // the arc's units in a degree
constexpr float DEFAULT_STEP_C = 0.5f;

lv_obj_t *s_dial         = nullptr;
lv_obj_t *s_dial_current = nullptr;
lv_obj_t *s_dial_target  = nullptr;
lv_obj_t *s_dial_mode    = nullptr;
lv_obj_t *s_dial_toggles[kDialToggleCount] = {};

float s_dial_step     = DEFAULT_STEP_C;
bool  s_dial_dragging = false;

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
    const int tenths = static_cast<int>(celsius * TENTHS_PER_DEGREE + 0.5f);
    std::snprintf(text, sizeof(text), with_unit ? "%d.%d °C" : "%d.%d", tenths / TENTHS_PER_DEGREE,
                  tenths % TENTHS_PER_DEGREE);
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

void build_thermostat(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
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

// The toggles and the dial, as the home model has them.
void paint_thermostat()
{
    const HomeState &home = home_state();
    for (int index = 0; index < kDialToggleCount; ++index) {
        lv_obj_t         *chip   = s_dial_toggles[index];
        const ToggleState &toggle = home.toggles[index];
        if (chip == nullptr) {
            continue;
        }
        const bool empty = toggle.label[0] == '\0';
        lv_obj_set_hidden(chip, empty);
        if (!empty) {
            lv_obj_t *text = lv_obj_get_child(chip, 0);
            theme::set_text(text, toggle.label);
            theme::center_ink(text);
            lv_obj_set_state(chip, LV_STATE_CHECKED, toggle.on);
            theme::set_text_color(text, toggle.on ? theme::text : theme::secondary);
            lv_obj_set_style_text_opa(text, toggle.on ? static_cast<lv_opa_t>(LV_OPA_COVER) : theme::mark_opa, 0);
        }
    }

    const ThermostatState &thermostat = home.thermostat;
    if (s_dial == nullptr) {
        return;
    }
    static float s_range_shown[3] = {};
    if (thermostat.ranged && (s_range_shown[0] != thermostat.min_c || s_range_shown[1] != thermostat.max_c ||
                              s_range_shown[2] != thermostat.step_c)) {
        s_range_shown[0] = thermostat.min_c;
        s_range_shown[1] = thermostat.max_c;
        s_range_shown[2] = thermostat.step_c;
        lv_arc_set_range(s_dial, static_cast<int>(thermostat.min_c * DIAL_SCALE),
                         static_cast<int>(thermostat.max_c * DIAL_SCALE));
        s_dial_step = thermostat.step_c > 0.0f ? thermostat.step_c : DEFAULT_STEP_C;
    }
    if (!thermostat.known) {
        return;
    }
    write_temperature(s_dial_current, thermostat.current_c, true);
    if (!s_dial_dragging) {
        write_temperature(s_dial_target, thermostat.target_c, true);
        if (thermostat.target_c >= 0.0f) {
            lv_arc_set_value(s_dial, static_cast<int>(thermostat.target_c * DIAL_SCALE + 0.5f));
        }
    }
    paint_dial(thermostat.state);
    lv_obj_set_state(s_dial_mode, LV_STATE_CHECKED, thermostat.state != Hvac::Off);
    lv_obj_t *mode_text = lv_obj_get_child(s_dial_mode, 0);
    theme::set_text(mode_text, thermostat.mode[0] != '\0' ? thermostat.mode : "--");
    theme::set_text_color(mode_text, theme::text);
}
}  // namespace

void build_thermostat_card(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    build_thermostat(parent, w, h);
    subscribe(Topic::Home, kNoView, paint_thermostat);
}

}  // namespace ui::detail
