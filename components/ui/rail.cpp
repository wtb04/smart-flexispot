#include "ui_internal.h"

namespace ui::detail {
namespace {
void clock_blink(lv_timer_t *)
{
    if (!s_clock_known) {
        return;
    }
    const lv_opa_t now = lv_obj_get_style_opa(s_clock_colon, LV_PART_MAIN);
    lv_obj_set_style_opa(s_clock_colon, now == LV_OPA_COVER ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

void move_event_cb(lv_event_t *e)
{
    if (s_handlers.move == nullptr) {
        return;
    }
    const auto direction =
        static_cast<Move>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    s_handlers.move(lv_event_get_code(e) == LV_EVENT_PRESSED ? direction : Move::Stop);
}

void create_move_button(lv_obj_t *parent, const char *symbol, Move direction, std::int32_t w,
                        std::int32_t h)
{
    lv_obj_t *btn = theme::make_button(parent, symbol, theme::panel_light,
                                       fonts::size_48());
    lv_obj_set_size(btn, w, h);

    auto *user_data = reinterpret_cast<void *>(
        static_cast<std::intptr_t>(std::to_underlying(direction)));
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_RELEASED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESS_LOST, user_data);
    register_desk_control(btn);
}

lv_obj_t *icon_bar(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                   std::int32_t h)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    theme::style_panel(bar, theme::panel, 2);
    theme::fill_accent(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::text), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(bar, lv_color_hex(theme::text), LV_STATE_PRESSED);
    lv_obj_set_clickable(bar, false);
    return bar;
}

void add_desk_icon(lv_obj_t *button, bool high)
{
    lv_obj_t *icon = lv_obj_create(button);
    lv_obj_set_size(icon, 58, 56);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 20, 0);
    theme::style_panel(icon, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(icon, false);

    const std::int32_t top = high ? 7 : 20;
    icon_bar(icon, 2, top, 54, 7);
    icon_bar(icon, 8, top + 7, 6, 45 - top);
    icon_bar(icon, 44, top + 7, 6, 45 - top);
    icon_bar(icon, 4, 49, 14, 5);
    icon_bar(icon, 40, 49, 14, 5);
}

void place_strip()
{
    const bool right = s_rail_right;
    theme::align(s_clock_box, right ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    theme::align(s_wifi_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    theme::align(s_phone_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, right ? 44 : -44,
                 0);
}

lv_obj_t *make_rail_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = theme::make_button(parent, text);
    lv_obj_set_size(btn, RAIL_CARD_W - 2 * PANEL_PAD, RAIL_BTN_H);
    theme::fill_accent(btn, LV_STATE_CHECKED);
    register_desk_control(btn);
    return btn;
}

void preset_clicked_cb(lv_event_t *e);
}  // namespace

lv_obj_t *s_preset_buttons[kPresetCount] = {};
bool      s_preset_active[kPresetCount]  = {};
namespace {
void bind_preset(lv_obj_t *button, int index)
{
    s_preset_buttons[index] = button;
    lv_obj_add_event_cb(button, preset_clicked_cb, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_add_event_cb(button, preset_clicked_cb, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
}

void preset_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    const bool store = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    if (!store && index >= 0 && index < kPresetCount && s_preset_active[index]) {
        return;
    }
    if (s_handlers.preset != nullptr) {
        s_handlers.preset(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))),
                    lv_event_get_code(e) == LV_EVENT_LONG_PRESSED);
    }
}

void manual_clicked_cb(lv_event_t *);
}  // namespace

lv_obj_t *s_drawer        = nullptr;
namespace {
lv_obj_t *s_drawer_toggle = nullptr;
bool      s_drawer_open   = false;
}  // namespace

void create_rail(lv_obj_t *parent)
{
    const Layout l = layout();

    lv_obj_t *rail = lv_obj_create(parent);
    s_rail         = rail;
    lv_obj_set_pos(rail, l.rail_right ? l.rail_x : GAP, GAP);
    lv_obj_set_size(rail, RAIL_CARD_W, l.screen_h - 2 * GAP);
    theme::style_panel(rail, theme::panel, theme::radius::card);
    lv_obj_set_style_pad_all(rail, PANEL_PAD, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rail, BUTTON_GAP, 0);

    lv_obj_t *strip = lv_obj_create(rail);
    lv_obj_set_size(strip, RAIL_CARD_W - 2 * PANEL_PAD, 44);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_scrollable(strip, false);

    lv_obj_t *clock = lv_obj_create(strip);
    s_clock_box     = clock;
    lv_obj_set_size(clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    theme::style_panel(clock, theme::panel, 0);
    lv_obj_set_style_bg_opa(clock, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(clock, false);
    lv_obj_set_flex_flow(clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(clock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(clock, 0, 0);

    s_clock_hours   = theme::make_label(clock, "--", theme::text, fonts::size_28());
    s_clock_colon   = theme::make_label(clock, ":", theme::text, fonts::size_28());
    lv_obj_set_style_pad_left(s_clock_colon, 4, 0);
    lv_obj_set_style_pad_right(s_clock_colon, 4, 0);
    s_clock_minutes = theme::make_label(clock, "--", theme::text, fonts::size_28());
    lv_timer_create(clock_blink, 1000, nullptr);

    auto status_icon = [&](const lv_image_dsc_t *src) {
        lv_obj_t *icon = lv_image_create(strip);
        lv_image_set_src(icon, src);
        lv_obj_set_style_image_recolor(icon, lv_color_hex(theme::text), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_clickable(icon, false);
        return icon;
    };
    s_wifi_icon  = status_icon(&icons::wifi_off_icon);
    s_phone_icon = status_icon(&icons::phone_off_icon);
    place_strip();

    lv_obj_t *heading = theme::make_label(rail, "DESK HEIGHT", theme::secondary,
                                          &lv_font_montserrat_18);
    lv_obj_set_style_margin_top(heading, 18, 0);

    lv_obj_t *readout = lv_obj_create(rail);
    lv_obj_set_size(readout, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    theme::style_panel(readout, theme::panel, 0);
    lv_obj_set_style_bg_opa(readout, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(readout, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(readout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_bottom(readout, 18, 0);

    s_height.emplace(readout);
    s_height->set_tenths(-1);

    theme::make_label(readout, "CM", theme::secondary, fonts::size_22());

    lv_obj_t *stand = make_rail_button(rail, "STAND");
    add_desk_icon(stand, true);
    lv_obj_align(lv_obj_get_child(stand, 0), LV_ALIGN_CENTER, 34, 0);
    bind_preset(stand, 2);

    lv_obj_t *sit = make_rail_button(rail, "SIT");
    add_desk_icon(sit, false);
    lv_obj_align(lv_obj_get_child(sit, 0), LV_ALIGN_CENTER, 34, 0);
    bind_preset(sit, 3);

    s_drawer_toggle = theme::make_button(rail, LV_SYMBOL_RIGHT);
    lv_obj_set_size(s_drawer_toggle, 84, 64);
    lv_obj_set_ignore_layout(s_drawer_toggle, true);
    lv_obj_align(s_drawer_toggle, l.rail_right ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0,
                 0);
    lv_obj_add_event_cb(s_drawer_toggle, manual_clicked_cb, LV_EVENT_CLICKED, nullptr);
}
namespace {
constexpr std::uint32_t DRAWER_MS = 200;
}  // namespace

// A card of its own beside the rail: tucked under it, it showed through the
// rail's rounded corners.
void place_drawer(std::int32_t width)
{
    const Layout l = layout();
    lv_obj_set_width(s_drawer, width);
    lv_obj_set_x(s_drawer, l.rail_right ? l.screen_w - RAIL_W - GAP - width : RAIL_W + GAP);
    lv_obj_set_hidden(s_drawer, width <= 0);
}
namespace {
void pad_drawer() { lv_obj_set_style_pad_all(s_drawer, PANEL_PAD, 0); }

void drawer_width_cb(void *, std::int32_t value) { place_drawer(value); }

void animate_drawer(bool open)
{
    s_drawer_open = open;
    if (s_drawer_toggle != nullptr) {
        theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                        open != s_rail_right ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_drawer);
    lv_anim_set_exec_cb(&anim, drawer_width_cb);
    lv_anim_set_values(&anim, lv_obj_get_width(s_drawer), open ? DRAWER_W : 0);
    lv_anim_set_duration(&anim, DRAWER_MS);
    lv_anim_set_path_cb(&anim, open ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&anim);
}

void manual_clicked_cb(lv_event_t *) { animate_drawer(!s_drawer_open); }
}  // namespace

void create_drawer(lv_obj_t *parent)
{
    const Layout l = layout();

    s_drawer = lv_obj_create(parent);
    lv_obj_set_y(s_drawer, GAP);
    lv_obj_set_height(s_drawer, l.screen_h - 2 * GAP);
    place_drawer(0);
    theme::style_panel(s_drawer, theme::panel, theme::radius::card);
    // It lies over the same surface, so a ring of background makes the card gap.
    lv_obj_set_style_outline_width(s_drawer, GAP, 0);
    lv_obj_set_style_outline_color(s_drawer, lv_color_hex(theme::background), 0);
    lv_obj_set_style_outline_opa(s_drawer, LV_OPA_COVER, 0);
    pad_drawer();
    lv_obj_set_flex_flow(s_drawer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_drawer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_drawer, BUTTON_GAP, 0);

    create_move_button(s_drawer, LV_SYMBOL_UP, Move::Up, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
    create_move_button(s_drawer, LV_SYMBOL_DOWN, Move::Down, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);

    // 3 and 4 are Stand and Sit on the rail.
    for (const int index : {0, 1, 4, 5}) {
        char label[24];
        std::snprintf(label, sizeof(label), "%s", preset_name(index));
        for (char *c = label; *c != '\0'; ++c) {
            *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        }
        lv_obj_t *btn = theme::make_button(s_drawer, label);
        lv_obj_set_size(btn, DRAWER_W - 2 * PANEL_PAD, RAIL_BTN_H);
        theme::fill_accent(btn, LV_STATE_CHECKED);
        bind_preset(btn, index);
        register_desk_control(btn);
    }
    show_guest_presets();
}
namespace {
void place_for_side()
{
    const Layout l = layout();
    lv_obj_set_pos(s_rail, l.rail_right ? l.rail_x : GAP, GAP);
    lv_obj_set_pos(s_content, l.content_x, GAP);
    pad_drawer();
    place_drawer(s_drawer_open ? DRAWER_W : 0);
    theme::align(s_drawer_toggle,
                 l.rail_right ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                    s_drawer_open != l.rail_right ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT);
    place_strip();
    lv_obj_set_pos(s_notice_scrim, l.rail_right ? 0 : RAIL_W, 0);
    lv_obj_align(s_notice_card, LV_ALIGN_CENTER, l.content_x + l.content_w / 2 - l.screen_w / 2,
                 GAP + l.content_h / 2 - l.screen_h / 2);
}
}  // namespace

void paint_choice(lv_obj_t *const buttons[2], bool second)
{
    for (int i = 0; i < 2; ++i) {
        const bool chosen = second == (i == 1);
        lv_obj_set_state(buttons[i], LV_STATE_CHECKED, chosen);
        theme::set_text_color(lv_obj_get_child(buttons[i], 0),
                              chosen ? theme::text : theme::secondary);
    }
}

void paint_side_buttons()
{
    paint_choice(s_side_buttons, s_rail_right);
}

void apply_rail_side(bool right)
{
    s_rail_right = right;
    place_for_side();
    paint_side_buttons();
}

}  // namespace ui::detail
