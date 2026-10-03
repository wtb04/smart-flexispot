#include "ui_internal.h"

#include "media_model.h"
#include "home_model.h"
#include "topics.h"

#include "esp_heap_caps.h"

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

void build_thermostat_card(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    build_thermostat(parent, 0, w, h);
}

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

// LV_LABEL_LONG_MODE_DOTS only truncates once the text exceeds the label's
// height, and a label left at content height simply grows.
void one_line(lv_obj_t *label, const lv_font_t *font, std::int32_t width)
{
    lv_obj_set_width(label, width);
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
}

// Favourites, from the bar's play while nothing plays: a cover each with its
// name, as many columns as there are favourites up to a row's worth.
constexpr std::int32_t PICK_COLUMNS  = 4;
constexpr std::int32_t PICKER_PAD      = theme::space::l;
constexpr std::int32_t PICKER_HEADER_H = 52;
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
    std::uint32_t  arts_shown;  // the model's count of its covers when last drawn
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

void paint_picks();

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
        s_pick_views[i].arts_shown = UINT32_MAX;
    }
    s_pick_picker->add_close_button();
    subscribe(Topic::Picks, kNoView, paint_picks);
}

// The favourites as the media model has them: those named, with their covers.
void paint_picks()
{
    for (int index = 0; index < media::kPickCount; ++index) {
        PickView   &view = s_pick_views[index];
        const Pick &pick = media_pick(index);
        if (view.root == nullptr) {
            continue;
        }
        view.named = pick.name[0] != '\0';
        theme::set_text(view.name, pick.name);
        lv_obj_set_hidden(view.root, !view.named);
        if (pick.arts == view.arts_shown) {
            continue;
        }
        view.arts_shown = pick.arts;
        if (pick.art != nullptr && view.rounded == nullptr) {
            view.rounded = static_cast<std::uint16_t *>(heap_caps_malloc(
                media::kPickArtSize * media::kPickArtSize * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        lv_obj_set_hidden(view.art, pick.art == nullptr || view.rounded == nullptr);
        if (pick.art == nullptr || view.rounded == nullptr) {
            continue;
        }
        // Rounded once here rather than clipped on every frame, which in software
        // costs more than the rest of the popup.
        round_corners(static_cast<const std::uint16_t *>(pick.art), view.rounded, media::kPickArtSize,
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
}

void open_pick_picker()
{
    const int count = media_pick_count();
    if (count == 0 || !s_pick_picker.has_value()) {
        return;
    }
    fit_pick_picker(count);
    s_pick_picker->open();
}
}  // namespace


void write_clock(lv_obj_t *label, int seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d:%02d", seconds / units::kSecondsPerMinute,
                  seconds % units::kSecondsPerMinute);
    theme::set_text(label, text);
}

void open_favourites()
{
    open_pick_picker();
}

namespace {
// The pills, the toggles and the thermostat, as the home model has them.
void paint_home()
{
    const HomeState &home = home_state();
    for (int index = 0; index < kPillCount; ++index) {
        Pill            &pill  = s_pills[index];
        const PillState &state = home.pills[index];
        if (pill.root == nullptr) {
            continue;
        }
        const bool empty = state.label[0] == '\0';
        lv_obj_set_hidden(pill.root, empty);
        if (pill.shown == empty) {
            pill.shown = !empty;
            reflow_pills();
        }
        if (!empty) {
            theme::set_text(pill.label, state.label);
            theme::set_text(pill.value, state.value[0] != '\0' ? state.value : "--");
            theme::set_bg_color(pill.dot, level_ink(state.level));
        }
    }
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

// The air along the top, and under it the radar as its page had it, the scope
// and the chosen aircraft's column, and what comes next beside them. What
// plays, the heating and the lights are the control bar's, so not here as well.
void build_home_page(lv_obj_t *page)
{
    const Layout l = layout();

    lv_obj_set_style_pad_all(page, PANEL_PAD, 0);
    const std::int32_t inner_w = l.content_w - 2 * PANEL_PAD;
    const std::int32_t inner_h = l.content_h - 2 * PANEL_PAD;

    build_pills(page, inner_w);

    const std::int32_t body_y  = PILL_H + BUTTON_GAP;
    const std::int32_t body_h  = inner_h - body_y;
    const std::int32_t radar_w = radar_cards_width(body_h);
    radar_home_area(page, 0, body_y, radar_w, body_h);
    build_next_tile(page, radar_w + BUTTON_GAP, body_y, inner_w - radar_w - BUTTON_GAP, body_h);

    build_pick_picker(lv_obj_get_screen(page));  // over the music view too, which is opened from there
    subscribe(Topic::Home, kNoView, paint_home);
}

}  // namespace ui::detail
