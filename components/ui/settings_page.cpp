#include "focus_model.h"
#include "diagnostics_model.h"
#include "settings_model.h"
#include "topics.h"
#include "ui_internal.h"

namespace ui::detail {
namespace {
constexpr std::int32_t SWATCH        = 128;
constexpr int          SWATCH_COLS   = 5;
constexpr std::int32_t SWATCH_RADIUS = 18;
constexpr std::int32_t ACCENT_DOT    = 44;
constexpr std::int32_t SIDE_BTN_W    = 150;
constexpr std::int32_t SIDE_BTN_H    = 60;

constexpr std::int32_t BACK_W        = 120;
constexpr std::int32_t SUB_TITLE_GAP = 20;
constexpr std::int32_t SUB_TITLE_X   = BACK_W + SUB_TITLE_GAP;
constexpr std::int32_t SUB_TITLE_Y   = 12;
constexpr std::int32_t SUB_BODY_Y    = DIAG_HEADER_H + BUTTON_GAP;
constexpr std::int32_t ROW_PITCH     = ROW_CARD_H + BUTTON_GAP;

// The Setup page's tiles: three ways into the sub-views, the diagnostics
// summary under them, and the screen and restart tiles at the foot.
constexpr int SETTINGS_TILE_ROWS = 3;
constexpr int PAGES_ROW          = 0;
constexpr int DIAGNOSTICS_ROW    = 1;
constexpr int POWER_ROW          = 2;
constexpr int PAGE_TILE_COLUMNS  = 3;
constexpr int APPEARANCE_COLUMN  = 0;
constexpr int BEHAVIOUR_COLUMN   = 1;
constexpr int FOCUS_COLUMN       = 2;

constexpr int          DIAG_COLUMNS = 3;
constexpr std::int32_t LOG_BUTTON_W = 150;

constexpr std::int32_t STEPPER_VALUE_W = 130;

constexpr int CHOICE_COUNT      = 2;
constexpr int ORIENTATION_COUNT = static_cast<int>(Orientation::Auto) + 1;

lv_obj_t *s_side_buttons[CHOICE_COUNT]      = {};
lv_obj_t *s_flip_buttons[ORIENTATION_COUNT] = {};
lv_obj_t *s_brightness_value                = nullptr;

void paint_pick(lv_obj_t *const *buttons, int count, int picked)
{
    for (int i = 0; i < count; ++i) {
        const bool chosen = i == picked;
        lv_obj_set_state(buttons[i], LV_STATE_CHECKED, chosen);
        theme::set_text_color(lv_obj_get_child(buttons[i], 0),
                              chosen ? theme::text : theme::secondary);
    }
}

void paint_choice(lv_obj_t *const buttons[CHOICE_COUNT], bool second)
{
    paint_pick(buttons, CHOICE_COUNT, second ? 1 : 0);
}

void brightness_event_cb(lv_event_t *e)
{
    auto      *slider  = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int  percent = static_cast<int>(lv_slider_get_value(slider));

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(s_brightness_value, text);

    if (s_handlers.brightness != nullptr) {
        s_handlers.brightness(percent);
    }
}

std::optional<ModalOverlay> s_colour_picker;
lv_obj_t                   *s_swatch_tick[theme::primaries.size()] = {};

void apply_primary(std::uint32_t colour)
{
    theme::set_primary(colour);
    for (std::size_t i = 0; i < theme::primaries.size(); ++i) {
        lv_obj_set_hidden(s_swatch_tick[i], theme::primaries[i] != colour);
    }
}

void swatch_clicked_cb(lv_event_t *e)
{
    const auto index =
        static_cast<std::size_t>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    apply_primary(theme::primaries[index]);
    if (s_colour_picker.has_value()) {
        s_colour_picker->close();
    }
    if (s_handlers.primary != nullptr) {
        s_handlers.primary(theme::primaries[index]);
    }
}

void accent_card_cb(lv_event_t *)
{
    if (s_colour_picker.has_value()) {
        s_colour_picker->open();
    }
}

void side_clicked_cb(lv_event_t *e)
{
    const bool right = lv_event_get_user_data(e) != nullptr;
    if (right == s_dock_right) {
        return;
    }
    s_dock_right = right;
    place_for_side();
    paint_choice(s_side_buttons, right);
    if (s_handlers.dock_side != nullptr) {
        s_handlers.dock_side(right);
    }
}

void flip_clicked_cb(lv_event_t *e)
{
    const auto picked = static_cast<Orientation>(
        reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (picked == s_orientation) {
        return;
    }
    s_orientation = picked;
    paint_pick(s_flip_buttons, ORIENTATION_COUNT, static_cast<int>(picked));
    if (s_handlers.orientation != nullptr) {
        s_handlers.orientation(picked);
    }
}

lv_obj_t *build_labelled_row_card(lv_obj_t *parent, std::int32_t y, std::int32_t w,
                                  const char *icon, const char *title, lv_event_cb_t clicked)
{
    lv_obj_t *root = build_row_card(parent, y, w, icon, clicked);
    lv_obj_set_flex_grow(theme::make_label(root, title, theme::secondary, fonts::size_22()), 1);
    return root;
}

void build_accent_card(lv_obj_t *parent, std::int32_t y, std::int32_t w)
{
    lv_obj_t *root =
        build_labelled_row_card(parent, y, w, LV_SYMBOL_TINT, "Accent colour", accent_card_cb);

    lv_obj_t *dot = lv_obj_create(root);
    lv_obj_set_size(dot, ACCENT_DOT, ACCENT_DOT);
    theme::style_panel(dot, theme::panel, ACCENT_DOT / 2);
    theme::fill_accent(dot);
    lv_obj_set_clickable(dot, false);

    theme::make_label(root, LV_SYMBOL_RIGHT, theme::secondary, fonts::size_28());
}

void build_choices(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                   const char *title, const char *const *labels, int count,
                   lv_event_cb_t clicked, lv_obj_t **out)
{
    lv_obj_t *root = build_labelled_row_card(parent, y, w, icon, title, nullptr);
    for (int i = 0; i < count; ++i) {
        lv_obj_t *btn = theme::make_button(root, labels[i], theme::panel, fonts::size_22());
        lv_obj_set_size(btn, SIDE_BTN_W, SIDE_BTN_H);
        theme::fill_accent(btn, LV_STATE_CHECKED);
        lv_obj_add_event_cb(btn, clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        out[i] = btn;
    }
}

void build_choice_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                       const char *title, const char *first, const char *second,
                       lv_event_cb_t clicked, lv_obj_t *out[CHOICE_COUNT])
{
    const char *const labels[CHOICE_COUNT] = {first, second};
    build_choices(parent, y, w, icon, title, labels, CHOICE_COUNT, clicked, out);
}

lv_obj_t *build_swatch(lv_obj_t *grid, std::size_t index)
{
    const std::uint32_t colour = theme::primaries[index];
    lv_obj_t           *btn    = lv_button_create(grid);
    lv_obj_set_size(btn, SWATCH, SWATCH);
    theme::style_button(btn, colour);
    lv_obj_set_style_radius(btn, SWATCH_RADIUS, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(theme::dim_of(colour)), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, swatch_clicked_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    lv_obj_t *tick = theme::make_label(btn, LV_SYMBOL_OK, theme::background, fonts::size_32());
    lv_obj_center(tick);
    lv_obj_set_hidden(tick, colour != theme::primary);
    return tick;
}

void build_colour_picker(lv_obj_t *parent)
{
    const int rows = (static_cast<int>(theme::primaries.size()) + SWATCH_COLS - 1) / SWATCH_COLS;
    const std::int32_t body_y = modal_body_y();
    const std::int32_t card_w =
        SWATCH_COLS * SWATCH + (SWATCH_COLS - 1) * BUTTON_GAP + 2 * DETAIL_PAD;
    const std::int32_t card_h = body_y + rows * SWATCH + (rows - 1) * BUTTON_GAP + 2 * DETAIL_PAD;

    s_colour_picker.emplace(parent, card_w, card_h);
    lv_obj_t *card = s_colour_picker->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    lv_obj_t *title = theme::make_accent_label(card, "Accent colour", fonts::size_28());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, MODAL_TITLE_Y);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_pos(grid, 0, body_y);
    lv_obj_set_size(grid, card_w - 2 * DETAIL_PAD, card_h - 2 * DETAIL_PAD - body_y);
    theme::style_panel(grid, theme::panel, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, BUTTON_GAP, 0);
    lv_obj_set_style_pad_column(grid, BUTTON_GAP, 0);

    for (std::size_t i = 0; i < theme::primaries.size(); ++i) {
        s_swatch_tick[i] = build_swatch(grid, i);
    }

    s_colour_picker->add_close_button();
}

lv_obj_t *build_sub_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = lv_obj_create(parent);
    lv_obj_set_pos(view, 0, 0);
    lv_obj_set_size(view, w, h);
    theme::style_panel(view, theme::panel, 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_TRANSP, 0);
    return view;
}

void build_sub_header(lv_obj_t *view, const char *title)
{
    lv_obj_t *back = lv_button_create(view);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_size(back, BACK_W, DIAG_HEADER_H);
    theme::style_button(back, theme::panel_light);
    lv_obj_center(theme::make_label(back, LV_SYMBOL_LEFT, theme::text, fonts::size_28()));
    lv_obj_add_event_cb(back, show_settings_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = theme::make_label(view, title, theme::text, fonts::size_28());
    lv_obj_set_pos(label, SUB_TITLE_X, SUB_TITLE_Y);
}

/** A sub-view under its header, hidden until its tile is tapped. */
lv_obj_t *build_hidden_sub_view(lv_obj_t *parent, std::int32_t w, std::int32_t h,
                                const char *title)
{
    lv_obj_t *view = build_sub_view(parent, w, h);
    lv_obj_set_hidden(view, true);
    build_sub_header(view, title);
    return view;
}

lv_obj_t *build_page_tile(lv_obj_t *parent, std::int32_t y, std::int32_t w, std::int32_t h,
                          const char *icon, const char *title, lv_event_cb_t clicked)
{
    lv_obj_t *tile = build_tile(parent, 0, y, w, h, icon, title, true);
    lv_obj_add_event_cb(tile, clicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *chevron = theme::make_label(tile, LV_SYMBOL_RIGHT, theme::secondary,
                                          fonts::size_28());
    lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, 0, 0);
    return tile;
}

}  // namespace

namespace {
// Three readings along the Diagnostics tile's foot, value over name, as the
// radar gives an aircraft's, clear of the chevron.
constexpr const char  *GLANCE_NAMES[kGlanceCount] = {"up", "Wi-Fi", "desk link"};
constexpr std::int32_t GLANCE_W        = 132;
constexpr std::int32_t GLANCE_CHEVRON  = 56;
lv_obj_t              *s_glance[kGlanceCount] = {};

void build_glances(lv_obj_t *tile)
{
    const std::int32_t name_h = theme::type_label()->line_height;
    for (int i = 0; i < kGlanceCount; ++i) {
        const std::int32_t right = GLANCE_CHEVRON + (kGlanceCount - 1 - i) * GLANCE_W;
        lv_obj_t *name = theme::make_label(tile, GLANCE_NAMES[i], theme::secondary,
                                           theme::type_label());
        lv_obj_set_width(name, GLANCE_W);
        lv_obj_align(name, LV_ALIGN_BOTTOM_RIGHT, -right, 0);
        s_glance[i] = theme::make_label(tile, "--", theme::text, theme::type_value());
        lv_obj_set_width(s_glance[i], GLANCE_W);
        lv_label_set_long_mode(s_glance[i], LV_LABEL_LONG_MODE_CLIP);
        lv_obj_align(s_glance[i], LV_ALIGN_BOTTOM_RIGHT, -right, -name_h);
    }
    subscribe(Topic::Diagnostics, kNoView, [] {
        for (int i = 0; i < kGlanceCount; ++i) {
            const char *value = diagnostics_state().glances[i].value;
            theme::set_text(s_glance[i], value[0] != '\0' ? value : "--");
        }
    });
}
}  // namespace


namespace {
// The restart tile doubles as the one that installs an update, while there is
// one: restarting into it is what installing is.
lv_obj_t *s_restart_tile = nullptr;
lv_obj_t *s_update_icon  = nullptr;  // in the glyph's place while updating
lv_obj_t *s_update_bar   = nullptr;  // along the foot, the time left after it
bool      s_update_ready = false;

constexpr std::int32_t UPDATE_BAR_H   = 6;
constexpr std::int32_t UPDATE_BAR_GAP = 16;
constexpr std::int32_t PERCENT_ALL    = 100;
constexpr char         LONGEST_LEFT[] = "00:00 left";

void update_clicked_cb(lv_event_t *)
{
    if (s_update_ready && s_handlers.update_now != nullptr) {
        s_handlers.update_now();
    }
}

void build_update_progress(lv_obj_t *tile, std::int32_t w)
{
    s_update_icon = lv_image_create(tile);
    lv_obj_set_style_image_recolor(s_update_icon, lv_color_hex(theme::primary), 0);
    lv_obj_set_style_image_recolor_opa(s_update_icon, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_update_icon, false);
    lv_obj_align(s_update_icon, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_hidden(s_update_icon, true);

    const lv_font_t   *font  = theme::type_body();
    const std::int32_t inner = w - 2 * lv_obj_get_style_pad_left(tile, LV_PART_MAIN);
    s_update_bar             = lv_bar_create(tile);
    lv_obj_set_size(s_update_bar, inner - theme::text_width(LONGEST_LEFT, font) - UPDATE_BAR_GAP,
                    UPDATE_BAR_H);
    lv_bar_set_range(s_update_bar, 0, PERCENT_ALL);
    theme::style_panel(s_update_bar, theme::panel, UPDATE_BAR_H / 2);
    theme::fill_accent(s_update_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_update_bar, UPDATE_BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_align(s_update_bar, LV_ALIGN_BOTTOM_LEFT, 0, -(font->line_height - UPDATE_BAR_H) / 2);
    lv_obj_set_hidden(s_update_bar, true);
}
}  // namespace

namespace {
lv_obj_t *s_behaviour_view = nullptr;
lv_obj_t *s_focus_view     = nullptr;

// The focus timer's plan, a stepper each: focus, break, long break, rounds.
struct Stepper {
    const char *icon;
    const char *title;
    const char *unit;
    int         step;
    int         low;
    int         high;
    int         value;
    lv_obj_t   *shown = nullptr;
    lv_obj_t   *less  = nullptr;
    lv_obj_t   *more  = nullptr;
};
constexpr int PLAN_WORK       = 0;
constexpr int PLAN_BREAK      = 1;
constexpr int PLAN_LONG_BREAK = 2;
constexpr int PLAN_ROUNDS     = 3;
constexpr int PLAN_STEPPERS   = 4;

constexpr int COARSE_STEP        = 5;
constexpr int FINE_STEP          = 1;
constexpr int BREAK_MIN_MAX      = 30;
constexpr int LONG_BREAK_MIN_MAX = 60;

Stepper s_plan[PLAN_STEPPERS] = {
    {.icon  = LV_SYMBOL_PLAY,
     .title = "Focus",
     .unit  = "min",
     .step  = COARSE_STEP,
     .low   = COARSE_STEP,
     .high  = FOCUS_WORK_MIN_MAX,
     .value = FOCUS_WORK_MIN_DEFAULT},
    {.icon  = LV_SYMBOL_PAUSE,
     .title = "Break",
     .unit  = "min",
     .step  = FINE_STEP,
     .low   = FINE_STEP,
     .high  = BREAK_MIN_MAX,
     .value = FOCUS_BREAK_MIN_DEFAULT},
    {.icon  = LV_SYMBOL_STOP,
     .title = "Long break",
     .unit  = "min",
     .step  = COARSE_STEP,
     .low   = COARSE_STEP,
     .high  = LONG_BREAK_MIN_MAX,
     .value = FOCUS_LONG_BREAK_MIN_DEFAULT},
    {.icon  = LV_SYMBOL_LOOP,
     .title = "Rounds before the long break",
     .unit  = "",
     .step  = FINE_STEP,
     .low   = FINE_STEP,
     .high  = FOCUS_ROUNDS_MAX,
     .value = FOCUS_ROUNDS_DEFAULT},
};

void paint_stepper(Stepper &stepper)
{
    if (stepper.shown == nullptr) {
        return;
    }
    char text[16];
    if (stepper.unit[0] != '\0') {
        std::snprintf(text, sizeof(text), "%d %s", stepper.value, stepper.unit);
    } else {
        std::snprintf(text, sizeof(text), "%d", stepper.value);
    }
    theme::set_text(stepper.shown, text);
    theme::set_usable(stepper.less, stepper.value > stepper.low);
    theme::set_usable(stepper.more, stepper.value < stepper.high);
}

void step(lv_event_t *e, int toward)
{
    Stepper &stepper = *static_cast<Stepper *>(lv_event_get_user_data(e));
    stepper.value = std::clamp(stepper.value + toward * stepper.step, stepper.low, stepper.high);
    paint_stepper(stepper);
    if (s_handlers.focus_plan != nullptr) {
        s_handlers.focus_plan(s_plan[PLAN_WORK].value, s_plan[PLAN_BREAK].value,
                              s_plan[PLAN_LONG_BREAK].value, s_plan[PLAN_ROUNDS].value);
    }
}

void less_clicked_cb(lv_event_t *e)
{
    step(e, -1);
}

void more_clicked_cb(lv_event_t *e)
{
    step(e, 1);
}

// Each setting is two ways, painted like the sidebar and orientation choices.
lv_obj_t *s_setting_choice[SETTING_COUNT][CHOICE_COUNT] = {};

void pick_setting(Setting setting, bool on)
{
    settings_pick(setting, on);
}

void gate_clicked_cb(lv_event_t *e)
{
    pick_setting(Setting::PresenceGate, lv_event_get_user_data(e) != nullptr);
}

void charge_clicked_cb(lv_event_t *e)
{
    pick_setting(Setting::Charging, lv_event_get_user_data(e) != nullptr);
}

void link_clicked_cb(lv_event_t *e)
{
    pick_setting(Setting::DeskBluetooth, lv_event_get_user_data(e) != nullptr);
}
}  // namespace

lv_obj_t *build_row_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                         lv_event_cb_t clicked)
{
    lv_obj_t *root = clicked != nullptr ? lv_button_create(parent) : lv_obj_create(parent);
    lv_obj_set_pos(root, 0, y);
    lv_obj_set_size(root, w, ROW_CARD_H);
    if (clicked != nullptr) {
        theme::style_button(root, theme::panel_light);
        lv_obj_add_event_cb(root, clicked, LV_EVENT_CLICKED, nullptr);
    } else {
        theme::style_panel(root, theme::panel_light, theme::radius::control);
    }
    lv_obj_set_style_pad_hor(root, ROW_CARD_PAD_X, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(root, ROW_CARD_GAP, 0);

    theme::make_accent_label(root, icon, fonts::size_28());
    return root;
}

// The choices and the volume, as the settings model has them.
void paint_settings()
{
    const SettingsState &settings = settings_state();
    for (int index = 0; index < SETTING_COUNT; ++index) {
        if (s_setting_choice[index][0] != nullptr) {
            paint_choice(s_setting_choice[index], settings.on[index]);
        }
    }
    if (s_volume_slider != nullptr && settings.notification_volume >= 0) {
        lv_slider_set_value(s_volume_slider, settings.notification_volume, LV_ANIM_OFF);
        write_percent(s_volume_value, settings.notification_volume);
    }
}

void show_diagnostics_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_settings_view, true);
    lv_obj_set_hidden(s_diag_view, false);
}

void show_appearance_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_settings_view, true);
    lv_obj_set_hidden(s_appearance_view, false);
}

void show_focus_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_settings_view, true);
    lv_obj_set_hidden(s_focus_view, false);
}

void show_behaviour_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_settings_view, true);
    lv_obj_set_hidden(s_behaviour_view, false);
}

void show_settings_cb(lv_event_t *)
{
    lv_obj_set_hidden(s_diag_view, true);
    lv_obj_set_hidden(s_appearance_view, true);
    lv_obj_set_hidden(s_behaviour_view, true);
    lv_obj_set_hidden(s_focus_view, true);
    lv_obj_set_hidden(s_settings_view, false);
}

namespace {
void build_settings_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);

    const std::int32_t tile_h =
        (h - (SETTINGS_TILE_ROWS - 1) * BUTTON_GAP) / SETTINGS_TILE_ROWS;
    const std::int32_t pitch  = tile_h + BUTTON_GAP;
    const std::int32_t half   = (w - BUTTON_GAP) / 2;
    const std::int32_t right  = half + BUTTON_GAP;
    const std::int32_t page_w = (w - (PAGE_TILE_COLUMNS - 1) * BUTTON_GAP) / PAGE_TILE_COLUMNS;
    const auto column_x = [page_w](int column) { return column * (page_w + BUTTON_GAP); };

    lv_obj_t *appearance = build_page_tile(view, PAGES_ROW * pitch, page_w, tile_h,
                                           LV_SYMBOL_IMAGE, "Appearance", show_appearance_cb);
    lv_obj_set_x(appearance, column_x(APPEARANCE_COLUMN));

    lv_obj_t *behave = build_page_tile(view, PAGES_ROW * pitch, page_w, tile_h, LV_SYMBOL_SETTINGS,
                                       "Behaviour", show_behaviour_cb);
    lv_obj_set_x(behave, column_x(BEHAVIOUR_COLUMN));

    lv_obj_t *focus = build_page_tile(view, PAGES_ROW * pitch, page_w, tile_h, LV_SYMBOL_LOOP,
                                      "Focus", show_focus_cb);
    lv_obj_set_x(focus, column_x(FOCUS_COLUMN));

    lv_obj_t *diag = build_page_tile(view, DIAGNOSTICS_ROW * pitch, w, tile_h, LV_SYMBOL_LIST,
                                     "Diagnostics", show_diagnostics_cb);
    s_diag_summary = tile_note(diag, w, "");
    build_glances(diag);

    lv_obj_t *screen = build_tile(view, 0, POWER_ROW * pitch, half, tile_h,
                                  LV_SYMBOL_EYE_CLOSE, "Screen off", true);
    lv_obj_add_event_cb(screen, screen_off_cb, LV_EVENT_CLICKED, nullptr);
    tile_note(screen, half, "Tap to wake");

    lv_obj_t *restart = build_tile(view, right, POWER_ROW * pitch, half, tile_h,
                                   LV_SYMBOL_POWER, "Restart", true);
    lv_obj_add_event_cb(restart, restart_held_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(restart, update_clicked_cb, LV_EVENT_CLICKED, nullptr);
    tile_note(restart, half, "Hold to restart");
    build_update_progress(restart, half);
    s_restart_tile = restart;

    s_settings_view = view;
}

void build_behaviour_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_hidden_sub_view(parent, w, h, "Behaviour");

    std::int32_t y = SUB_BODY_Y;
    // Off first, then on, as the settings count them.
    build_choice_card(view, y, w, LV_SYMBOL_EYE_OPEN, "Pages while away", "SHOW", "HIDE",
                      gate_clicked_cb,
                      s_setting_choice[static_cast<int>(Setting::PresenceGate)]);
    y += ROW_PITCH;
    build_choice_card(view, y, w, LV_SYMBOL_BATTERY_FULL, "Battery charging", "OFF", "ON",
                      charge_clicked_cb, s_setting_choice[static_cast<int>(Setting::Charging)]);
    y += ROW_PITCH;
    build_choice_card(view, y, w, LV_SYMBOL_UP, "Desk link", "WIRE", "BLUETOOTH",
                      link_clicked_cb,
                      s_setting_choice[static_cast<int>(Setting::DeskBluetooth)]);
    y += ROW_PITCH;
    s_volume_slider = build_slider_card(view, y, w, LV_SYMBOL_VOLUME_MAX, "Notification volume",
                                        0, 0, volume_changed_cb, &s_volume_value);
    subscribe(Topic::Settings, kNoView, paint_settings);
    subscribe(Topic::Update, kNoView, [] {
        if (settings_state().update_known) {
            paint_update_tile(settings_state().update);
        }
    });

    s_behaviour_view = view;
}

void build_stepper(lv_obj_t *view, std::int32_t y, std::int32_t w, Stepper &stepper)
{
    lv_obj_t *root = build_labelled_row_card(view, y, w, stepper.icon, stepper.title, nullptr);
    stepper.less = theme::make_chip(root, "");
    theme::make_mark(stepper.less, &icons::minus_icon);
    stepper.shown = theme::make_label(root, "", theme::text, fonts::size_28());
    lv_obj_set_width(stepper.shown, STEPPER_VALUE_W);
    lv_obj_set_style_text_align(stepper.shown, LV_TEXT_ALIGN_CENTER, 0);
    stepper.more = theme::make_chip(root, "");
    theme::make_mark(stepper.more, &icons::plus_icon);
    lv_obj_add_event_cb(stepper.less, less_clicked_cb, LV_EVENT_CLICKED, &stepper);
    lv_obj_add_event_cb(stepper.more, more_clicked_cb, LV_EVENT_CLICKED, &stepper);
    paint_stepper(stepper);
}

void build_focus_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_hidden_sub_view(parent, w, h, "Focus");
    for (int i = 0; i < PLAN_STEPPERS; ++i) {
        build_stepper(view, SUB_BODY_Y + i * ROW_PITCH, w, s_plan[i]);
    }
    s_focus_view = view;
    subscribe(Topic::Focus, kNoView, [] { paint_focus_plan(focus_state()); });
}

void build_appearance_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_hidden_sub_view(parent, w, h, "Appearance");

    std::int32_t y = SUB_BODY_Y;
    build_slider_card(view, y, w, LV_SYMBOL_EYE_OPEN, "Brightness", s_initial_brightness,
                      board::kMinBrightness, brightness_event_cb, &s_brightness_value);
    y += ROW_PITCH;
    build_accent_card(view, y, w);
    y += ROW_PITCH;
    build_choice_card(view, y, w, LV_SYMBOL_BARS, "Dock", "LEFT", "RIGHT", side_clicked_cb,
                      s_side_buttons);
    paint_choice(s_side_buttons, s_dock_right);
    y += ROW_PITCH;
    // Auto turns the screen to however the panel stands, by the IMU.
    const char *const facing[ORIENTATION_COUNT] = {"NORMAL", "FLIPPED", "AUTO"};
    build_choices(view, y, w, LV_SYMBOL_REFRESH, "Orientation", facing, ORIENTATION_COUNT,
                  flip_clicked_cb, s_flip_buttons);
    paint_pick(s_flip_buttons, ORIENTATION_COUNT, static_cast<int>(s_orientation));

    s_appearance_view = view;
}

void build_diagnostics_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_hidden_sub_view(parent, w, h, "Diagnostics");

    lv_obj_t *log = theme::make_button(view, LV_SYMBOL_FILE "  Log", theme::panel_light,
                                       fonts::size_22());
    lv_obj_set_size(log, LOG_BUTTON_W, DIAG_HEADER_H);
    lv_obj_align(log, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(log, show_log_cb, LV_EVENT_CLICKED, nullptr);

    const int          rows   = (s_card_count + DIAG_COLUMNS - 1) / DIAG_COLUMNS;
    const std::int32_t tile_w = (w - (DIAG_COLUMNS - 1) * BUTTON_GAP) / DIAG_COLUMNS;
    const std::int32_t tile_h = (h - SUB_BODY_Y - (rows - 1) * BUTTON_GAP) / rows;

    for (int i = 0; i < s_card_count; ++i) {
        build_info_tile(view, i, (i % DIAG_COLUMNS) * (tile_w + BUTTON_GAP),
                        SUB_BODY_Y + (i / DIAG_COLUMNS) * (tile_h + BUTTON_GAP), tile_w, tile_h);
    }

    s_diag_view = view;
}
}  // namespace

void build_settings_page(lv_obj_t *page)
{
    const Layout l = layout();

    lv_obj_set_style_pad_all(page, PANEL_PAD, 0);
    const std::int32_t inner_w = l.content_w - 2 * PANEL_PAD;
    const std::int32_t inner_h = l.content_h - 2 * PANEL_PAD;

    build_settings_view(page, inner_w, inner_h);
    build_appearance_view(page, inner_w, inner_h);
    build_behaviour_view(page, inner_w, inner_h);
    build_focus_view(page, inner_w, inner_h);
    build_diagnostics_view(page, inner_w, inner_h);
    build_detail_overlay(page);
    build_log_overlay(page);
    build_colour_picker(page);
    refresh_diag_summary();
    follow_diagnostics();
}

void paint_focus_plan(const Focus &focus)
{
    const int values[PLAN_STEPPERS] = {focus.work_min, focus.break_min, focus.long_break_min,
                                       focus.rounds};
    for (int i = 0; i < PLAN_STEPPERS; ++i) {
        if (s_plan[i].value != values[i]) {
            s_plan[i].value = values[i];
            paint_stepper(s_plan[i]);
        }
    }
}

void paint_update_tile(const UpdateState &state)
{
    if (s_restart_tile == nullptr) {
        return;
    }
    constexpr std::uint32_t GLYPH = 0, CAPTION = 1, NOTE = 2;
    lv_obj_t *glyph   = lv_obj_get_child(s_restart_tile, GLYPH);
    lv_obj_t *caption = lv_obj_get_child(s_restart_tile, CAPTION);
    lv_obj_t *note    = lv_obj_get_child(s_restart_tile, NOTE);

    const bool busy  = state.busy != UpdateTarget::None;
    const bool ready = state.panel_ready || state.companion_ready;
    s_update_ready   = ready && !busy;

    const char *what = state.panel_ready && state.companion_ready ? "Panel and companion ready"
                       : state.panel_ready                        ? "Panel ready"
                                                                  : "Companion ready";
    const char *doing = state.phase == UpdatePhase::Installing ? "Installing" : "Downloading";
    char        left[24] = "";
    if (state.seconds_left >= 0) {
        std::snprintf(left, sizeof(left), "%d:%02d left", state.seconds_left / units::kSecondsPerMinute,
                      state.seconds_left % units::kSecondsPerMinute);
    }
    lv_obj_set_hidden(glyph, busy);
    lv_obj_set_hidden(s_update_icon, !busy);
    lv_obj_set_hidden(s_update_bar, !busy);
    if (busy) {
        lv_image_set_src(s_update_icon, update_icon(state));
        lv_bar_set_value(s_update_bar, state.percent, LV_ANIM_OFF);
    }
    lv_obj_set_style_text_align(note, busy ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    theme::set_text(glyph, ready ? LV_SYMBOL_DOWNLOAD : LV_SYMBOL_POWER);
    theme::set_text(caption, busy ? doing : ready ? "Update now" : "Restart");
    theme::set_text(note, busy ? left : ready ? what : "Hold to restart");
    theme::fill_accent_or(s_restart_tile, s_update_ready, theme::panel_light);
    theme::set_text_color(glyph, s_update_ready ? theme::text : theme::primary);
    theme::set_text_color(note, s_update_ready ? theme::text : theme::secondary);
}

}  // namespace ui::detail
