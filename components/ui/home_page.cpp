#include "ui_internal.h"

namespace ui::detail {
namespace {
constexpr float DEFAULT_MIN_C  = 15.0f;
constexpr float DEFAULT_MAX_C  = 30.0f;

constexpr std::int32_t DIAL_CARD_W = 480;
constexpr std::int32_t DIAL_INSET    = 100;
constexpr std::int32_t DIAL_CHIP     = theme::chip::size;
constexpr std::int32_t DIAL_CHIP_GAP = 10;
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
    const int tenths = static_cast<int>(celsius * 10.0f + 0.5f);
    std::snprintf(text, sizeof(text), with_unit ? "%d.%d °C" : "%d.%d", tenths / 10, tenths % 10);
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
constexpr std::int32_t PILL_H = 60;
constexpr std::int32_t DOT    = 14;
}  // namespace

Pill         s_pills[kPillCount];
namespace {
std::int32_t s_pill_row_w = 0;

constexpr std::int32_t PILL_PAD = 22;
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
namespace {
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
    lv_obj_set_size(base, 16, BULB_H - BULB_W - 3);
    lv_obj_set_pos(base, (BULB_W - 16) / 2, BULB_W + 3);
    theme::style_panel(base, theme::disabled_ink, 3);
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
}  // namespace

lv_obj_t *s_panel_volume_pct  = nullptr;
namespace {
lv_obj_t *s_panel_volume_icon = nullptr;
lv_timer_t *s_progress_timer = nullptr;
}  // namespace

TextBox s_card_with_art{}, s_card_bare{};
TextBox s_panel_with_art{}, s_panel_bare{};
bool    s_has_art = false;
namespace {
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
}  // namespace

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
namespace {
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
}  // namespace

void write_clock(lv_obj_t *label, int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
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
}  // namespace

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

}  // namespace ui::detail
