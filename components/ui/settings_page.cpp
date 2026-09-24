#include "ui_internal.h"

namespace ui::detail {
namespace {
constexpr std::int32_t SWATCH      = 128;
constexpr int          SWATCH_COLS = 5;
constexpr std::int32_t ACCENT_DOT  = 44;
constexpr std::int32_t SIDE_BTN_W  = 150;
constexpr std::int32_t SIDE_BTN_H  = 60;

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
    if (right == s_rail_right) {
        return;
    }
    apply_rail_side(right);
    if (s_handlers.rail_side != nullptr) {
        s_handlers.rail_side(right);
    }
}

void flip_clicked_cb(lv_event_t *e)
{
    const bool flipped = lv_event_get_user_data(e) != nullptr;
    if (flipped == s_flipped) {
        return;
    }
    s_flipped = flipped;
    paint_choice(s_flip_buttons, flipped);
    if (s_handlers.orientation != nullptr) {
        s_handlers.orientation(flipped);
    }
}

lv_obj_t *build_row_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                         const char *title, lv_event_cb_t clicked)
{
    lv_obj_t *root = clicked != nullptr ? lv_button_create(parent) : lv_obj_create(parent);
    lv_obj_set_pos(root, 0, y);
    lv_obj_set_size(root, w, ROW_CARD_H);
    if (clicked != nullptr) {
        theme::style_button(root, theme::panel_light);
        lv_obj_add_event_cb(root, clicked, LV_EVENT_CLICKED, nullptr);
    } else {
        theme::style_panel(root, theme::panel_light, 14);
    }
    lv_obj_set_style_pad_hor(root, 22, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(root, 18, 0);

    theme::make_accent_label(root, icon, fonts::size_28());
    lv_obj_set_flex_grow(theme::make_label(root, title, theme::secondary, fonts::size_22()), 1);
    return root;
}

void build_accent_card(lv_obj_t *parent, std::int32_t y, std::int32_t w)
{
    lv_obj_t *root = build_row_card(parent, y, w, LV_SYMBOL_TINT, "Accent colour", accent_card_cb);

    lv_obj_t *dot = lv_obj_create(root);
    lv_obj_set_size(dot, ACCENT_DOT, ACCENT_DOT);
    theme::style_panel(dot, theme::panel, ACCENT_DOT / 2);
    theme::fill_accent(dot);
    lv_obj_set_clickable(dot, false);

    theme::make_label(root, LV_SYMBOL_RIGHT, theme::secondary, fonts::size_28());
}

void build_choice_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                       const char *title, const char *first, const char *second,
                       lv_event_cb_t clicked, lv_obj_t *out[2])
{
    lv_obj_t         *root    = build_row_card(parent, y, w, icon, title, nullptr);
    const char *const text[2] = {first, second};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *btn = theme::make_button(root, text[i], theme::panel, fonts::size_22());
        lv_obj_set_size(btn, SIDE_BTN_W, SIDE_BTN_H);
        theme::fill_accent(btn, LV_STATE_CHECKED);
        lv_obj_add_event_cb(btn, clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));
        out[i] = btn;
    }
}

void build_colour_picker(lv_obj_t *parent)
{
    const int rows = (static_cast<int>(theme::primaries.size()) + SWATCH_COLS - 1) / SWATCH_COLS;
    const std::int32_t body_y = ModalOverlay::header_height() + HEADER_GAP;
    const std::int32_t card_w =
        SWATCH_COLS * SWATCH + (SWATCH_COLS - 1) * BUTTON_GAP + 2 * DETAIL_PAD;
    const std::int32_t card_h = body_y + rows * SWATCH + (rows - 1) * BUTTON_GAP + 2 * DETAIL_PAD;

    s_colour_picker.emplace(parent, card_w, card_h);
    lv_obj_t *card = s_colour_picker->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    lv_obj_t *title = theme::make_accent_label(card, "Accent colour", fonts::size_28());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 6);

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
        const std::uint32_t colour = theme::primaries[i];
        lv_obj_t           *btn    = lv_button_create(grid);
        lv_obj_set_size(btn, SWATCH, SWATCH);
        theme::style_button(btn, colour);
        lv_obj_set_style_radius(btn, 18, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(theme::dim_of(colour)), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, swatch_clicked_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(i)));

        lv_obj_t *tick = theme::make_label(btn, LV_SYMBOL_OK, theme::background, fonts::size_32());
        lv_obj_center(tick);
        lv_obj_set_hidden(tick, colour != theme::primary);
        s_swatch_tick[i] = tick;
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
    lv_obj_set_size(back, 120, DIAG_HEADER_H);
    theme::style_button(back, theme::panel_light);
    lv_obj_center(theme::make_label(back, LV_SYMBOL_LEFT, theme::text, fonts::size_28()));
    lv_obj_add_event_cb(back, show_settings_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = theme::make_label(view, title, theme::text, fonts::size_28());
    lv_obj_set_pos(label, 140, 12);
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
lv_obj_t *s_behaviour_view = nullptr;

// Each setting is two ways, painted like the sidebar and orientation choices.
lv_obj_t *s_setting_choice[SETTING_COUNT][2] = {};
bool      s_setting_on[SETTING_COUNT]        = {};

void pick_setting(Setting setting, bool on)
{
    const int index = static_cast<int>(setting);
    if (on == s_setting_on[index]) {
        return;
    }
    apply_setting(index, on);
    if (s_handlers.setting != nullptr) {
        s_handlers.setting(setting, on);
    }
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

void apply_setting(int index, bool on)
{
    s_setting_on[index] = on;
    if (s_setting_choice[index][0] != nullptr) {
        paint_choice(s_setting_choice[index], on);
    }
    if (static_cast<Setting>(index) == Setting::PresenceGate) {
        s_presence_gate = on;
        select_page(s_page);
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
    lv_obj_set_hidden(s_settings_view, false);
}

namespace {
void build_settings_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);

    s_volume_slider = build_slider_card(view, 0, w, LV_SYMBOL_VOLUME_MAX, "Notification volume", 0,
                                        0, volume_changed_cb, &s_volume_value);

    const std::int32_t tiles_y = ROW_CARD_H + BUTTON_GAP;
    const std::int32_t tile_h  = (h - tiles_y - 2 * BUTTON_GAP) / 3;
    const std::int32_t pitch   = tile_h + BUTTON_GAP;
    const std::int32_t half    = (w - BUTTON_GAP) / 2;
    const std::int32_t right   = half + BUTTON_GAP;

    lv_obj_t *look = build_page_tile(view, tiles_y, half, tile_h, LV_SYMBOL_IMAGE, "Appearance",
                                     show_appearance_cb);
    tile_note(look, half, "Colour, layout, brightness");

    lv_obj_t *behave = build_page_tile(view, tiles_y, half, tile_h, LV_SYMBOL_SETTINGS,
                                       "Behaviour", show_behaviour_cb);
    lv_obj_set_x(behave, right);
    tile_note(behave, half, "Presence, charging, desk link");

    lv_obj_t *diag = build_page_tile(view, tiles_y + pitch, half, tile_h, LV_SYMBOL_LIST,
                                     "Diagnostics", show_diagnostics_cb);
    s_diag_summary = tile_note(diag, half, "");

    lv_obj_t *log = build_page_tile(view, tiles_y + pitch, half, tile_h, LV_SYMBOL_FILE, "Log",
                                    show_log_cb);
    lv_obj_set_x(log, right);
    tile_note(log, half, "Everything, newest last");

    lv_obj_t *screen = build_tile(view, 0, tiles_y + 2 * pitch, half, tile_h,
                                  LV_SYMBOL_EYE_CLOSE, "Screen off", true);
    lv_obj_add_event_cb(screen, screen_off_cb, LV_EVENT_CLICKED, nullptr);
    tile_note(screen, half, "Tap anywhere to bring it back");

    lv_obj_t *restart = build_tile(view, right, tiles_y + 2 * pitch, half, tile_h,
                                   LV_SYMBOL_POWER, "Restart", true);
    lv_obj_add_event_cb(restart, restart_held_cb, LV_EVENT_LONG_PRESSED, nullptr);
    tile_note(restart, half, "Hold to restart");

    s_settings_view = view;
}

void build_behaviour_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);
    lv_obj_set_hidden(view, true);
    build_sub_header(view, "Behaviour");

    const std::int32_t body_y = DIAG_HEADER_H + BUTTON_GAP;
    const std::int32_t pitch  = ROW_CARD_H + BUTTON_GAP;

    // Off first, then on, as the settings count them.
    build_choice_card(view, body_y, w, LV_SYMBOL_EYE_OPEN, "Pages while away", "SHOW", "HIDE",
                      gate_clicked_cb,
                      s_setting_choice[static_cast<int>(Setting::PresenceGate)]);
    build_choice_card(view, body_y + pitch, w, LV_SYMBOL_BATTERY_FULL, "Battery charging", "OFF",
                      "ON", charge_clicked_cb,
                      s_setting_choice[static_cast<int>(Setting::Charging)]);
    build_choice_card(view, body_y + 2 * pitch, w, LV_SYMBOL_UP, "Desk link", "WIRE",
                      "BLUETOOTH", link_clicked_cb,
                      s_setting_choice[static_cast<int>(Setting::DeskBluetooth)]);
    for (int i = 0; i < SETTING_COUNT; ++i) {
        apply_setting(i, s_setting_on[i]);
    }

    s_behaviour_view = view;
}

void build_appearance_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);
    lv_obj_set_hidden(view, true);
    build_sub_header(view, "Appearance");

    const std::int32_t body_y = DIAG_HEADER_H + BUTTON_GAP;
    const std::int32_t pitch = ROW_CARD_H + BUTTON_GAP;

    build_slider_card(view, body_y, w, LV_SYMBOL_EYE_OPEN, "Brightness", s_initial_brightness,
                      board::kMinBrightness, brightness_event_cb, &s_brightness_value);
    build_accent_card(view, body_y + pitch, w);
    build_choice_card(view, body_y + 2 * pitch, w, LV_SYMBOL_BARS, "Sidebar", "LEFT", "RIGHT",
                      side_clicked_cb, s_side_buttons);
    paint_side_buttons();
    build_choice_card(view, body_y + 3 * pitch, w, LV_SYMBOL_REFRESH, "Orientation", "NORMAL",
                      "FLIPPED", flip_clicked_cb, s_flip_buttons);
    paint_choice(s_flip_buttons, s_flipped);

    s_appearance_view = view;
}

void build_diagnostics_view(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *view = build_sub_view(parent, w, h);
    lv_obj_set_hidden(view, true);
    build_sub_header(view, "Diagnostics");

    const std::int32_t grid_y = DIAG_HEADER_H + BUTTON_GAP;
    const int          rows   = (s_card_count + 2) / 3;
    const std::int32_t tile_w = (w - 2 * BUTTON_GAP) / 3;
    const std::int32_t tile_h = (h - grid_y - (rows - 1) * BUTTON_GAP) / rows;

    for (int i = 0; i < s_card_count; ++i) {
        build_info_tile(view, i, (i % 3) * (tile_w + BUTTON_GAP),
                        grid_y + (i / 3) * (tile_h + BUTTON_GAP), tile_w, tile_h);
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
    build_diagnostics_view(page, inner_w, inner_h);
    build_detail_overlay(page);
    build_log_overlay(page);
    build_colour_picker(page);
    refresh_diag_summary();
}

}  // namespace ui::detail
