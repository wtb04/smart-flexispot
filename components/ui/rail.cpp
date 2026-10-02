#include "ui_internal.h"

#include "room_model.h"
#include "topics.h"

#include <climits>
#include <utility>

namespace ui::detail {
namespace {
constexpr std::int32_t RAIL_INNER_W = RAIL_CARD_W - 2 * PANEL_PAD;

constexpr std::int32_t  STRIP_H           = 44;
constexpr std::int32_t  PHONE_ICON_OFFSET = 44;
constexpr std::int32_t  UPDATE_ICON_OFFSET = 2 * PHONE_ICON_OFFSET;
constexpr std::int32_t  UPDATE_ICON_SIDE   = 28;
constexpr std::int32_t  UPDATE_BAR_H       = 3;
constexpr std::int32_t  UPDATE_BAR_GAP     = 2;
constexpr std::int32_t  UPDATE_BAR_Y       = UPDATE_ICON_SIDE / 2 + UPDATE_BAR_GAP + UPDATE_BAR_H / 2;
constexpr std::int32_t  PERCENT_ALL        = 100;

lv_obj_t *s_update_icon = nullptr;  // while an update arrives, for whichever board
lv_obj_t *s_update_bar  = nullptr;  // how far it is, under the icon
constexpr std::uint32_t COLON_BLINK_MS    = 1000;

// Above the heading and below the reading, setting the desk's height apart.
constexpr std::int32_t HEIGHT_BLOCK_MARGIN = 18;

// Stand and Sit are on the rail.
constexpr int DRAWER_PRESETS[] = {0, 1, 4, 5};

constexpr std::int32_t PRESET_LABEL_SHIFT = 34;  // past the desk drawn beside it

constexpr std::int32_t DRAWER_TOGGLE_W = 84;
constexpr std::int32_t DRAWER_TOGGLE_H = 64;
constexpr std::int32_t DRAWER_INNER_W  = DRAWER_W - 2 * PANEL_PAD;

// The desk drawn on Stand and Sit: a top, two legs and two feet.
constexpr std::int32_t DESK_ICON_W = 58;  // the size its pictures are drawn at
constexpr std::int32_t DESK_ICON_H = 56;
constexpr std::int32_t DESK_ICON_X = 20;

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

// The desk raised or lowered, in the accent until pressed or the preset in use.
// The picture sits in a box of its own, since those states are handed down to
// the button's grandchildren.
void add_desk_icon(lv_obj_t *button, bool high)
{
    lv_obj_t *icon = lv_obj_create(button);
    lv_obj_set_size(icon, DESK_ICON_W, DESK_ICON_H);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, DESK_ICON_X, 0);
    theme::style_panel(icon, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(icon, false);

    lv_obj_t *desk = lv_image_create(icon);
    lv_image_set_src(desk, high ? &icons::desk_up_icon : &icons::desk_down_icon);
    lv_obj_set_pos(desk, 0, 0);
    lv_obj_set_style_image_recolor_opa(desk, LV_OPA_COVER, 0);
    theme::tint_accent(desk);
    lv_obj_set_style_image_recolor(desk, lv_color_hex(theme::text), LV_STATE_CHECKED);
    lv_obj_set_style_image_recolor(desk, lv_color_hex(theme::text), LV_STATE_PRESSED);
    lv_obj_set_clickable(desk, false);
}

void place_strip()
{
    const bool right = s_rail_right;
    theme::align(s_clock_box, right ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    theme::align(s_wifi_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
    theme::align(s_phone_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 right ? PHONE_ICON_OFFSET : -PHONE_ICON_OFFSET, 0);
    theme::align(s_update_icon, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 right ? UPDATE_ICON_OFFSET : -UPDATE_ICON_OFFSET, 0);
    theme::align(s_update_bar, right ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID,
                 right ? UPDATE_ICON_OFFSET : -UPDATE_ICON_OFFSET, UPDATE_BAR_Y);
}

lv_obj_t *make_rail_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *btn = theme::make_button(parent, text);
    lv_obj_set_size(btn, RAIL_INNER_W, RAIL_BTN_H);
    theme::fill_accent(btn, LV_STATE_CHECKED);
    register_desk_control(btn);
    return btn;
}

void preset_clicked_cb(lv_event_t *e);
}  // namespace

lv_obj_t *s_preset_buttons[kPresetCount] = {};
namespace {
void bind_preset(lv_obj_t *button, int index)
{
    s_preset_buttons[index] = button;
    auto *user_data         = reinterpret_cast<void *>(static_cast<std::intptr_t>(index));
    lv_obj_add_event_cb(button, preset_clicked_cb, LV_EVENT_SHORT_CLICKED, user_data);
    lv_obj_add_event_cb(button, preset_clicked_cb, LV_EVENT_LONG_PRESSED, user_data);
}

void preset_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    const bool store = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED;
    if (!store && index >= 0 && index < kPresetCount && desk_state().preset_active[index]) {
        return;
    }
    if (s_handlers.preset != nullptr) {
        s_handlers.preset(index, store);
    }
}

void add_preset_button(lv_obj_t *rail, const char *name, bool high, int index)
{
    lv_obj_t *button = make_rail_button(rail, name);
    add_desk_icon(button, high);
    lv_obj_align(lv_obj_get_child(button, 0), LV_ALIGN_CENTER, PRESET_LABEL_SHIFT, 0);
    bind_preset(button, index);
}

void manual_clicked_cb(lv_event_t *);

lv_obj_t *make_status_icon(lv_obj_t *strip, const lv_image_dsc_t *src)
{
    lv_obj_t *icon = lv_image_create(strip);
    lv_image_set_src(icon, src);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(theme::text), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_clickable(icon, false);
    return icon;
}

void build_clock(lv_obj_t *strip)
{
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
    lv_obj_set_style_pad_left(s_clock_colon, theme::space::xs, 0);
    lv_obj_set_style_pad_right(s_clock_colon, theme::space::xs, 0);
    s_clock_minutes = theme::make_label(clock, "--", theme::text, fonts::size_28());
    lv_timer_create(clock_blink, COLON_BLINK_MS, nullptr);
}

void build_status_strip(lv_obj_t *rail)
{
    lv_obj_t *strip = lv_obj_create(rail);
    lv_obj_set_size(strip, RAIL_INNER_W, STRIP_H);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_scrollable(strip, false);

    build_clock(strip);
    s_wifi_icon   = make_status_icon(strip, &icons::wifi_off_icon);
    s_phone_icon  = make_status_icon(strip, &icons::phone_off_icon);
    s_update_icon = make_status_icon(strip, &icons::update_panel_icon);
    lv_obj_set_hidden(s_update_icon, true);
    s_update_bar = lv_bar_create(strip);
    lv_obj_set_size(s_update_bar, UPDATE_ICON_SIDE, UPDATE_BAR_H);
    lv_bar_set_range(s_update_bar, 0, PERCENT_ALL);
    theme::style_panel(s_update_bar, theme::panel_light, UPDATE_BAR_H / 2);
    theme::fill_accent(s_update_bar, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_update_bar, UPDATE_BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_hidden(s_update_bar, true);
    place_strip();
}

void build_height_readout(lv_obj_t *rail)
{
    lv_obj_t *heading = theme::make_label(rail, "DESK HEIGHT", theme::secondary,
                                          &lv_font_montserrat_18);
    lv_obj_set_style_margin_top(heading, HEIGHT_BLOCK_MARGIN, 0);

    lv_obj_t *readout = lv_obj_create(rail);
    lv_obj_set_size(readout, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    theme::style_panel(readout, theme::panel, 0);
    lv_obj_set_style_bg_opa(readout, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(readout, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(readout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_bottom(readout, HEIGHT_BLOCK_MARGIN, 0);

    s_height.emplace(readout);
    s_height->set_tenths(-1);

    theme::make_label(readout, "CM", theme::secondary, fonts::size_22());
}

lv_align_t drawer_toggle_corner(bool rail_right)
{
    return rail_right ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT;
}

const char *drawer_toggle_arrow(bool open, bool rail_right)
{
    return open != rail_right ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT;
}
}  // namespace

lv_obj_t *s_drawer        = nullptr;
namespace {
lv_obj_t *s_drawer_frame  = nullptr;  // clips the drawer to beside the rail as it slides
lv_obj_t *s_drawer_toggle = nullptr;
bool      s_drawer_open   = false;
}  // namespace

namespace {
// Each preset button lit while the desk stands at its height.
void paint_presets()
{
    for (int index = 0; index < kPresetCount; ++index) {
        lv_obj_t  *button = s_preset_buttons[index];
        const bool active = desk_state().preset_active[index];
        if (button == nullptr || lv_obj_has_state(button, LV_STATE_CHECKED) == active) {
            continue;
        }
        lv_obj_set_state(button, LV_STATE_CHECKED, active);
        for (std::uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
            lv_obj_t *child = lv_obj_get_child(button, i);
            theme::set_text_color(child, theme::text);
            for (std::uint32_t j = 0; j < lv_obj_get_child_count(child); ++j) {
                lv_obj_set_state(lv_obj_get_child(child, j), LV_STATE_CHECKED, active);
            }
        }
    }
}
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

    build_status_strip(rail);
    build_height_readout(rail);
    add_preset_button(rail, "STAND", true, STAND_PRESET);
    add_preset_button(rail, "SIT", false, SIT_PRESET);

    s_drawer_toggle = theme::make_button(rail, LV_SYMBOL_RIGHT);
    lv_obj_set_size(s_drawer_toggle, DRAWER_TOGGLE_W, DRAWER_TOGGLE_H);
    lv_obj_set_ignore_layout(s_drawer_toggle, true);
    lv_obj_align(s_drawer_toggle, drawer_toggle_corner(l.rail_right), 0, 0);
    lv_obj_add_event_cb(s_drawer_toggle, manual_clicked_cb, LV_EVENT_CLICKED, nullptr);
    subscribe(Topic::Desk, kNoView, [] {
        paint_presets();
        paint_desk_shortcuts();
        static int s_height_shown = INT_MIN;
        if (s_height.has_value() && std::exchange(s_height_shown, desk_state().height_mm) != desk_state().height_mm) {
            s_height->set_tenths(desk_state().height_mm);
        }
    });
}
namespace {
constexpr std::uint32_t DRAWER_MS = 200;

// A card of its own beside the rail, slid rather than resized: resizing it laid
// every button out again on every frame. It slides inside a frame that starts
// at the rail's edge, so it comes out from beside the rail and never over its
// rounded corners, which tucking it under the rail showed through.
constexpr std::int32_t DRAWER_FRAME_W = DRAWER_W + 2 * GAP;
}  // namespace

void place_drawer(std::int32_t shown)
{
    const Layout l = layout();
    lv_obj_set_x(s_drawer_frame, l.rail_right ? l.rail_x - DRAWER_FRAME_W : GAP + RAIL_W);
    const std::int32_t tucked = DRAWER_W + GAP - shown;
    lv_obj_set_x(s_drawer, l.rail_right ? GAP + tucked : GAP - tucked);
    lv_obj_set_hidden(s_drawer_frame, shown <= 0);
}
namespace {
void pad_drawer() { lv_obj_set_style_pad_all(s_drawer, PANEL_PAD, 0); }

std::int32_t s_drawer_shown = 0;  // how far it has come out, 0 to DRAWER_W

void drawer_shown_cb(void *, std::int32_t value)
{
    s_drawer_shown = value;
    place_drawer(value);
}

void animate_drawer(bool open)
{
    s_drawer_open = open;
    if (s_drawer_toggle != nullptr) {
        theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                        drawer_toggle_arrow(open, s_rail_right));
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_drawer);
    lv_anim_set_exec_cb(&anim, drawer_shown_cb);
    lv_anim_set_values(&anim, s_drawer_shown, open ? DRAWER_W : 0);
    lv_anim_set_duration(&anim, DRAWER_MS);
    lv_anim_set_path_cb(&anim, open ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&anim);
}

void manual_clicked_cb(lv_event_t *) { animate_drawer(!s_drawer_open); }

void build_drawer_frame(lv_obj_t *parent, std::int32_t screen_h)
{
    s_drawer_frame = lv_obj_create(parent);
    lv_obj_set_pos(s_drawer_frame, 0, 0);
    lv_obj_set_size(s_drawer_frame, DRAWER_FRAME_W, screen_h);
    theme::style_panel(s_drawer_frame, theme::panel, 0);
    lv_obj_set_style_bg_opa(s_drawer_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(s_drawer_frame, false);
}

void build_drawer_card(std::int32_t screen_h)
{
    s_drawer = lv_obj_create(s_drawer_frame);
    lv_obj_set_y(s_drawer, GAP);
    lv_obj_set_size(s_drawer, DRAWER_W, screen_h - 2 * GAP);
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
}

void add_drawer_preset(int index)
{
    char label[24];
    std::snprintf(label, sizeof(label), "%s", preset_name(index));
    for (char *c = label; *c != '\0'; ++c) {
        *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    }
    lv_obj_t *btn = theme::make_button(s_drawer, label);
    lv_obj_set_size(btn, DRAWER_INNER_W, RAIL_BTN_H);
    theme::fill_accent(btn, LV_STATE_CHECKED);
    bind_preset(btn, index);
    register_desk_control(btn);
}
}  // namespace

void create_drawer(lv_obj_t *parent)
{
    const Layout l = layout();

    build_drawer_frame(parent, l.screen_h);
    build_drawer_card(l.screen_h);

    create_move_button(s_drawer, LV_SYMBOL_UP, Move::Up, DRAWER_INNER_W, RAIL_BTN_H);
    create_move_button(s_drawer, LV_SYMBOL_DOWN, Move::Down, DRAWER_INNER_W, RAIL_BTN_H);
    for (const int index : DRAWER_PRESETS) {
        add_drawer_preset(index);
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
    theme::align(s_drawer_toggle, drawer_toggle_corner(l.rail_right), 0, 0);
    theme::set_text(lv_obj_get_child(s_drawer_toggle, 0),
                    drawer_toggle_arrow(s_drawer_open, l.rail_right));
    place_strip();
    lv_obj_set_pos(s_notice_scrim, l.rail_right ? 0 : RAIL_W, 0);
    lv_obj_align(s_notice_card, LV_ALIGN_CENTER, l.content_x + l.content_w / 2 - l.screen_w / 2,
                 GAP + l.content_h / 2 - l.screen_h / 2);
}
}  // namespace

void paint_pick(lv_obj_t *const *buttons, int count, int picked)
{
    for (int i = 0; i < count; ++i) {
        const bool chosen = i == picked;
        lv_obj_set_state(buttons[i], LV_STATE_CHECKED, chosen);
        theme::set_text_color(lv_obj_get_child(buttons[i], 0),
                              chosen ? theme::text : theme::secondary);
    }
}

void paint_choice(lv_obj_t *const buttons[2], bool second)
{
    paint_pick(buttons, 2, second ? 1 : 0);
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

const lv_image_dsc_t *update_icon(const UpdateState &state)
{
    if (state.busy == UpdateTarget::Panel) {
        return &icons::update_panel_icon;
    }
    return state.phase == UpdatePhase::Installing ? &icons::install_companion_icon
                                                  : &icons::update_companion_icon;
}

void paint_update_icon(const UpdateState &state)
{
    if (s_update_icon == nullptr) {
        return;
    }
    const bool busy = state.busy != UpdateTarget::None;
    lv_obj_set_hidden(s_update_icon, !busy);
    lv_obj_set_hidden(s_update_bar, !busy);
    if (busy) {
        // Accent for one that installs, and restarts, as soon as it is in.
        lv_obj_set_style_image_recolor(
            s_update_icon, lv_color_hex(state.immediate ? theme::primary : theme::text), 0);
        lv_image_set_src(s_update_icon, update_icon(state));
        lv_bar_set_value(s_update_bar, state.percent, LV_ANIM_OFF);
    }
}

namespace {
constexpr std::int32_t SHORTCUT_GAP       = theme::space::m;
constexpr std::int32_t SHORTCUT_ICON      = 30;  // the desk drawn small in a chip
constexpr int          SHORTCUT_PRESETS[] = {STAND_PRESET, SIT_PRESET};
constexpr std::size_t  SHORTCUT_COUNT     = std::size(SHORTCUT_PRESETS);
constexpr int          SHORTCUT_SETS      = kDeskShortcutButtons / static_cast<int>(SHORTCUT_COUNT);

struct Shortcuts {
    lv_obj_t *box                   = nullptr;
    lv_obj_t *chips[SHORTCUT_COUNT] = {};
    lv_obj_t *marks[SHORTCUT_COUNT] = {};
};
Shortcuts s_shortcuts[SHORTCUT_SETS];
int       s_shortcut_sets = 0;
}  // namespace

void paint_desk_shortcuts()
{
    for (int i = 0; i < s_shortcut_sets; ++i) {
        for (std::size_t j = 0; j < SHORTCUT_COUNT; ++j) {
            const bool active = desk_state().preset_active[SHORTCUT_PRESETS[j]];
            lv_obj_set_state(s_shortcuts[i].chips[j], LV_STATE_CHECKED, active);
            lv_obj_set_state(s_shortcuts[i].marks[j], LV_STATE_CHECKED, active);
        }
    }
}

lv_obj_t *add_desk_shortcuts(lv_obj_t *root, std::int32_t x, std::int32_t y, std::uint32_t chip_colour)
{
    if (s_shortcut_sets == SHORTCUT_SETS) {
        return nullptr;
    }
    Shortcuts &set = s_shortcuts[s_shortcut_sets++];
    set.box        = lv_obj_create(root);
    theme::style_panel(set.box, theme::panel, 0);
    lv_obj_set_style_bg_opa(set.box, LV_OPA_TRANSP, 0);
    lv_obj_set_size(set.box, static_cast<std::int32_t>(SHORTCUT_COUNT) * (theme::chip::size + SHORTCUT_GAP) - SHORTCUT_GAP,
                    theme::chip::size);
    lv_obj_set_pos(set.box, x, y);
    lv_obj_set_clickable(set.box, false);
    for (std::size_t i = 0; i < SHORTCUT_COUNT; ++i) {
        lv_obj_t *chip = theme::make_chip(set.box, "");
        lv_obj_set_pos(chip, static_cast<std::int32_t>(i) * (theme::chip::size + SHORTCUT_GAP), 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(chip_colour), 0);
        lv_obj_set_ext_click_area(chip, SHORTCUT_GAP / 2);
        theme::fill_accent(chip, LV_STATE_CHECKED);
        lv_obj_t *mark = theme::make_mark(chip, SHORTCUT_PRESETS[i] == STAND_PRESET ? &icons::desk_up_icon
                                                                                  : &icons::desk_down_icon);
        lv_image_set_scale(mark, LV_SCALE_NONE * SHORTCUT_ICON / DESK_ICON_W);
        lv_obj_set_style_image_recolor(mark, lv_color_hex(theme::text), LV_STATE_CHECKED);
        lv_obj_set_style_image_opa(mark, LV_OPA_COVER, LV_STATE_CHECKED);
        // A tap goes there as one on the rail does; storing a height stays on the rail.
        lv_obj_add_event_cb(chip, preset_clicked_cb, LV_EVENT_SHORT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(SHORTCUT_PRESETS[i])));
        register_desk_control(chip);
        set.chips[i] = chip;
        set.marks[i] = mark;
    }
    paint_desk_shortcuts();
    return set.box;
}

}  // namespace ui::detail
