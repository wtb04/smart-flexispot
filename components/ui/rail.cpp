#include "ui_internal.h"

#include "room_model.h"
#include "status_model.h"
#include "topics.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <utility>

// The dock at the panel's side: the tabs, and at its foot Stand and Sit and the
// height, which folds the rest of the desk out beside it: its height in the
// segment digits, every preset, and up and down to hold.
namespace ui::detail {
namespace {
constexpr std::int32_t DOCK_INNER_W = DOCK_W - 2 * 12;
constexpr std::int32_t DOCK_PAD     = (DOCK_W - DOCK_INNER_W) / 2;
constexpr std::int32_t DOCK_DESK_H  = 72;   // Stand and Sit, square to the finger
constexpr std::int32_t DOCK_HEIGHT_H = 48;  // the height under them
constexpr std::int32_t DOCK_DESK_ICON = 40;

constexpr std::int32_t SHEET_PAD     = 24;
constexpr std::int32_t SHEET_INNER_W = DRAWER_W - 2 * SHEET_PAD;
constexpr std::int32_t SHEET_COLS    = 2;
constexpr std::int32_t SHEET_BTN_W   = (SHEET_INNER_W - (SHEET_COLS - 1) * BUTTON_GAP) / SHEET_COLS;
constexpr std::int32_t SHEET_BTN_H   = 76;  // until those showing share the height
constexpr lv_opa_t     SHEET_DIM     = LV_OPA_60;  // the page under it
constexpr std::uint32_t SHEET_MS     = 200;
// Folded out, it goes again once left alone this long.
constexpr std::uint32_t SHEET_IDLE_MS = 20 * 1000;

// Every preset in the fold-out, Stand and Sit first as the dock has them.
constexpr int SHEET_PRESETS[] = {STAND_PRESET, SIT_PRESET, ULTRA_LOW_PRESET, 0, 4, 5};
// Presets 5 and 6 are for whoever uses the desk while the phone is away.
constexpr int GUEST_PRESETS[] = {4, 5};

constexpr std::int32_t DESK_ICON_W = 58;  // the size the desk's pictures are drawn at

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
    lv_obj_t *btn = theme::make_button(parent, symbol, theme::panel_light, fonts::size_32());
    lv_obj_set_size(btn, w, h);

    auto *user_data = reinterpret_cast<void *>(
        static_cast<std::intptr_t>(std::to_underlying(direction)));
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESSED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_RELEASED, user_data);
    lv_obj_add_event_cb(btn, move_event_cb, LV_EVENT_PRESS_LOST, user_data);
    register_desk_control(btn);
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
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) {
        desk_go_to(index);
    } else if (s_handlers.preset != nullptr) {
        s_handlers.preset(index, true);  // held: the height it is at, stored there
    }
}

constexpr std::int32_t  TRAVEL_RING = 3;    // round a preset the desk is on its way to
constexpr std::uint32_t TRAVEL_PULSE_MS = 700;

void set_ring_opa(void *obj, std::int32_t opa)
{
    lv_obj_set_style_border_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(opa), 0);
}
}  // namespace

// A ring in the accent that breathes while the desk is on its way there.
void show_desk_travel(lv_obj_t *obj, bool travelling)
{
    if (obj == nullptr || (lv_obj_get_style_border_width(obj, LV_PART_MAIN) > 0) == travelling) {
        return;
    }
    lv_anim_delete(obj, set_ring_opa);
    lv_obj_set_style_border_width(obj, travelling ? TRAVEL_RING : 0, 0);
    if (!travelling) {
        return;
    }
    lv_obj_set_style_border_color(obj, lv_color_hex(theme::primary), 0);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, set_ring_opa);
    lv_anim_set_values(&anim, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_duration(&anim, TRAVEL_PULSE_MS);
    lv_anim_set_playback_duration(&anim, TRAVEL_PULSE_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

namespace {
// Each preset button lit while the desk stands at its height.
void paint_presets()
{
    for (int index = 0; index < kPresetCount; ++index) {
        lv_obj_t  *button = s_preset_buttons[index];
        const bool active = desk_state().preset_active[index];
        show_desk_travel(button, desk_state().travelling == index);
        if (button == nullptr || lv_obj_has_state(button, LV_STATE_CHECKED) == active) {
            continue;
        }
        lv_obj_set_state(button, LV_STATE_CHECKED, active);
        theme::set_text_color(lv_obj_get_child(button, 0), theme::text);
    }
}

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

// Stand and Sit, each a button with the desk drawn on it, lit while the desk
// is there: round chips over the fullscreen views, squares down the dock.
Shortcuts *add_shortcuts(lv_obj_t *root, bool column, std::int32_t side, std::int32_t radius,
                         std::int32_t mark_w, std::uint32_t colour)
{
    if (s_shortcut_sets == SHORTCUT_SETS) {
        return nullptr;
    }
    Shortcuts &set = s_shortcuts[s_shortcut_sets++];
    set.box        = lv_obj_create(root);
    lv_obj_remove_style_all(set.box);
    const std::int32_t length = static_cast<std::int32_t>(SHORTCUT_COUNT) * (side + SHORTCUT_GAP) - SHORTCUT_GAP;
    lv_obj_set_size(set.box, column ? side : length, column ? length : side);
    lv_obj_set_clickable(set.box, false);
    for (std::size_t i = 0; i < SHORTCUT_COUNT; ++i) {
        lv_obj_t *chip = theme::make_chip(set.box, "");
        lv_obj_set_size(chip, side, side);
        lv_obj_set_style_radius(chip, radius, 0);
        const std::int32_t at = static_cast<std::int32_t>(i) * (side + SHORTCUT_GAP);
        lv_obj_set_pos(chip, column ? 0 : at, column ? at : 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(colour), 0);
        lv_obj_set_ext_click_area(chip, SHORTCUT_GAP / 2);
        theme::fill_accent(chip, LV_STATE_CHECKED);
        lv_obj_t *mark = theme::make_mark(chip, SHORTCUT_PRESETS[i] == STAND_PRESET ? &icons::desk_up_icon
                                                                                  : &icons::desk_down_icon);
        lv_image_set_scale(mark, LV_SCALE_NONE * mark_w / DESK_ICON_W);
        lv_obj_set_style_image_recolor(mark, lv_color_hex(theme::text), LV_STATE_CHECKED);
        lv_obj_set_style_image_opa(mark, LV_OPA_COVER, LV_STATE_CHECKED);
        auto *preset = reinterpret_cast<void *>(static_cast<std::intptr_t>(SHORTCUT_PRESETS[i]));
        lv_obj_add_event_cb(chip, preset_clicked_cb, LV_EVENT_SHORT_CLICKED, preset);
        if (column) {
            // Held, it stores the height as the fold-out's do; over a view only taps.
            lv_obj_add_event_cb(chip, preset_clicked_cb, LV_EVENT_LONG_PRESSED, preset);
        }
        register_desk_control(chip);
        set.chips[i] = chip;
        set.marks[i] = mark;
    }
    paint_desk_shortcuts();
    return &set;
}

lv_obj_t *s_tabs        = nullptr;  // the dock's column of tabs
lv_obj_t *s_dock_height = nullptr;  // the height, small, under Stand and Sit
lv_obj_t *s_height_button = nullptr;  // around it: folds the desk out
lv_obj_t *s_sheet_grid  = nullptr;  // the presets
lv_obj_t *s_sheet_moves = nullptr;  // up and down
lv_obj_t *s_sheet_scrim = nullptr;  // under the fold-out: a tap anywhere else folds it away
lv_obj_t *s_sheet       = nullptr;
bool      s_sheet_open  = false;
lv_timer_t *s_sheet_idle = nullptr;

void build_dock_desk(lv_obj_t *dock)
{
    lv_obj_t *foot = lv_obj_create(dock);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, DOCK_INNER_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(foot, BUTTON_GAP, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_clickable(foot, false);

    add_shortcuts(foot, true, DOCK_INNER_W, theme::radius::control, DOCK_DESK_ICON, theme::panel_light);

    // The height, as the dock has room for it; a tap folds the rest of the desk out.
    lv_obj_t *height = lv_button_create(foot);
    s_height_button  = height;
    lv_obj_remove_style_all(height);
    lv_obj_set_size(height, DOCK_INNER_W, DOCK_HEIGHT_H);
    lv_obj_set_style_radius(height, theme::radius::control, 0);
    for (const lv_state_t state : {LV_STATE_PRESSED, LV_STATE_CHECKED}) {
        lv_obj_set_style_bg_color(height, lv_color_hex(theme::panel_light), state);
        lv_obj_set_style_bg_opa(height, LV_OPA_COVER, state);
    }
    lv_obj_add_event_cb(height, [](lv_event_t *) { open_desk_sheet(!s_sheet_open); }, LV_EVENT_CLICKED, nullptr);
    s_dock_height = theme::make_label(height, "--", theme::primary, fonts::size_22());
    lv_obj_center(s_dock_height);
}

void show_dock_height(int tenths)
{
    char text[12] = "--";
    if (tenths >= 0) {
        std::snprintf(text, sizeof(text), "%d.%d", tenths / 10, tenths % 10);
    }
    theme::set_text(s_dock_height, text);
}

void place_sheet(std::int32_t shown)
{
    const Layout l = layout();
    // From under the dock: its edge at the dock's while folded away, out by `shown`.
    const std::int32_t edge = l.rail_right ? l.rail_x - GAP : l.rail_x + DOCK_W + GAP;
    lv_obj_set_x(s_sheet, l.rail_right ? edge - shown : edge - DRAWER_W + shown);
    lv_obj_set_hidden(s_sheet, shown <= 0);
}

void sheet_shown_cb(void *, std::int32_t value)
{
    place_sheet(value);
}

void build_sheet_heading(lv_obj_t *sheet)
{
    theme::make_label(sheet, "DESK HEIGHT", theme::secondary, &lv_font_montserrat_18);
    lv_obj_t *readout = lv_obj_create(sheet);
    lv_obj_remove_style_all(readout);
    lv_obj_set_size(readout, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(readout, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(readout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(readout, BUTTON_GAP, 0);
    s_height.emplace(readout);
    s_height->set_tenths(-1);
    theme::make_label(readout, "CM", theme::secondary, fonts::size_22());
}

void add_sheet_preset(lv_obj_t *grid, int index)
{
    char label[24];
    std::snprintf(label, sizeof(label), "%s", preset_name(index));
    for (char *c = label; *c != '\0'; ++c) {
        *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    }
    lv_obj_t *btn = theme::make_button(grid, label, theme::panel_light, fonts::size_22());
    lv_obj_set_size(btn, SHEET_BTN_W, SHEET_BTN_H);
    theme::fill_accent(btn, LV_STATE_CHECKED);
    bind_preset(btn, index);
    register_desk_control(btn);
}
// The rows of buttons showing share what the fold-out has under its height.
void fit_sheet()
{
    int showing = 0;
    for (const int index : SHEET_PRESETS) {
        showing += lv_obj_is_hidden(s_preset_buttons[index]) ? 0 : 1;
    }
    const int rows = (showing + SHEET_COLS - 1) / SHEET_COLS + 1;
    lv_obj_update_layout(s_sheet);
    const std::int32_t room = lv_obj_get_height(s_sheet) - SHEET_PAD - lv_obj_get_y(s_sheet_grid);
    const std::int32_t height = (room - rows * BUTTON_GAP) / rows;
    for (const int index : SHEET_PRESETS) {
        lv_obj_set_height(s_preset_buttons[index], height);
    }
    lv_obj_set_height(s_sheet_moves, height);
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(s_sheet_moves); ++i) {
        lv_obj_set_height(lv_obj_get_child(s_sheet_moves, static_cast<std::int32_t>(i)), height);
    }
}
}  // namespace

void show_guest_presets()
{
    if (s_sheet == nullptr) {
        return;
    }
    const bool shown = !s_presence_gate || !status_state().present;
    for (const int index : GUEST_PRESETS) {
        lv_obj_set_hidden(s_preset_buttons[index], !shown);
    }
    fit_sheet();
}

void paint_desk_shortcuts()
{
    for (int i = 0; i < s_shortcut_sets; ++i) {
        for (std::size_t j = 0; j < SHORTCUT_COUNT; ++j) {
            const bool active = desk_state().preset_active[SHORTCUT_PRESETS[j]];
            show_desk_travel(s_shortcuts[i].chips[j], desk_state().travelling == SHORTCUT_PRESETS[j]);
            lv_obj_set_state(s_shortcuts[i].chips[j], LV_STATE_CHECKED, active);
            lv_obj_set_state(s_shortcuts[i].marks[j], LV_STATE_CHECKED, active);
        }
    }
}

lv_obj_t *add_desk_shortcuts(lv_obj_t *root, std::int32_t x, std::int32_t y, std::uint32_t chip_colour)
{
    Shortcuts *set = add_shortcuts(root, false, theme::chip::size, theme::chip::size / 2, SHORTCUT_ICON, chip_colour);
    if (set == nullptr) {
        return nullptr;
    }
    lv_obj_set_pos(set->box, x, y);
    return set->box;
}

lv_obj_t *dock_tabs()
{
    return s_tabs;
}

void create_dock(lv_obj_t *parent)
{
    const Layout l = layout();
    lv_obj_t    *dock = lv_obj_create(parent);
    s_rail            = dock;
    lv_obj_set_pos(dock, l.rail_x, GAP);
    lv_obj_set_size(dock, DOCK_W, l.screen_h - 2 * GAP);
    theme::style_panel(dock, theme::panel, theme::radius::card);
    lv_obj_set_style_pad_all(dock, DOCK_PAD, 0);
    lv_obj_set_scrollable(dock, false);

    s_tabs = lv_obj_create(dock);
    lv_obj_remove_style_all(s_tabs);
    lv_obj_set_size(s_tabs, DOCK_INNER_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_tabs, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_tabs, BUTTON_GAP / 2, 0);
    lv_obj_align(s_tabs, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(s_tabs, false);

    build_dock_desk(dock);
    subscribe(Topic::Desk, kNoView, [] {
        paint_presets();
        paint_desk_shortcuts();
        static int s_height_shown = INT_MIN;
        const int  height         = desk_state().height_mm;
        if (std::exchange(s_height_shown, height) != height) {
            show_dock_height(height);
            if (s_height.has_value()) {
                s_height->set_tenths(height);
            }
        }
    });
}

void open_desk_sheet(bool open)
{
    if (s_sheet == nullptr) {
        return;
    }
    s_sheet_open = open;
    lv_obj_set_hidden(s_sheet_scrim, !open);
    lv_obj_set_state(s_height_button, LV_STATE_CHECKED, open);
    if (open) {
        lv_obj_move_foreground(s_sheet_scrim);
        lv_obj_move_foreground(s_sheet);
        lv_obj_move_foreground(s_rail);
        lv_timer_reset(s_sheet_idle);
        lv_timer_resume(s_sheet_idle);
    } else {
        lv_timer_pause(s_sheet_idle);
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_sheet);
    lv_anim_set_exec_cb(&anim, sheet_shown_cb);
    lv_anim_set_values(&anim, open ? 0 : DRAWER_W + GAP, open ? DRAWER_W + GAP : 0);
    lv_anim_set_duration(&anim, SHEET_MS);
    lv_anim_set_path_cb(&anim, open ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&anim);
}

void create_desk_sheet(lv_obj_t *parent)
{
    const Layout l = layout();
    s_sheet_scrim  = lv_obj_create(parent);
    lv_obj_remove_style_all(s_sheet_scrim);
    lv_obj_set_size(s_sheet_scrim, l.screen_w, l.screen_h);
    lv_obj_set_style_bg_color(s_sheet_scrim, lv_color_hex(theme::background), 0);
    lv_obj_set_style_bg_opa(s_sheet_scrim, SHEET_DIM, 0);
    lv_obj_add_event_cb(s_sheet_scrim, [](lv_event_t *) { open_desk_sheet(false); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_hidden(s_sheet_scrim, true);

    s_sheet = lv_obj_create(parent);
    lv_obj_set_size(s_sheet, DRAWER_W, l.screen_h - 2 * GAP);
    lv_obj_set_y(s_sheet, GAP);
    theme::style_panel(s_sheet, theme::panel, theme::radius::card);
    // It lies over the page, so a ring of background sets it apart.
    lv_obj_set_style_outline_width(s_sheet, GAP, 0);
    lv_obj_set_style_outline_color(s_sheet, lv_color_hex(theme::background), 0);
    lv_obj_set_style_outline_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_sheet, SHEET_PAD, 0);
    lv_obj_set_scrollable(s_sheet, false);
    lv_obj_set_flex_flow(s_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sheet, BUTTON_GAP, 0);
    lv_obj_set_style_pad_top(s_sheet, SHEET_PAD + theme::space::s, 0);
    // A press on it keeps it out as long as it is being used.
    lv_obj_add_event_cb(s_sheet, [](lv_event_t *) { lv_timer_reset(s_sheet_idle); }, LV_EVENT_PRESSED, nullptr);

    build_sheet_heading(s_sheet);
    lv_obj_t *grid = lv_obj_create(s_sheet);
    s_sheet_grid   = grid;
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, SHEET_INNER_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, BUTTON_GAP, 0);
    lv_obj_set_style_pad_column(grid, BUTTON_GAP, 0);
    lv_obj_set_clickable(grid, false);
    for (const int index : SHEET_PRESETS) {
        add_sheet_preset(grid, index);
    }
    lv_obj_t *moves = lv_obj_create(s_sheet);
    s_sheet_moves   = moves;
    lv_obj_remove_style_all(moves);
    lv_obj_set_size(moves, SHEET_INNER_W, SHEET_BTN_H);
    lv_obj_set_flex_flow(moves, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(moves, BUTTON_GAP, 0);
    lv_obj_set_clickable(moves, false);
    create_move_button(moves, LV_SYMBOL_UP, Move::Up, SHEET_BTN_W, SHEET_BTN_H);
    create_move_button(moves, LV_SYMBOL_DOWN, Move::Down, SHEET_BTN_W, SHEET_BTN_H);

    s_sheet_idle = lv_timer_create([](lv_timer_t *) { open_desk_sheet(false); }, SHEET_IDLE_MS, nullptr);
    lv_timer_pause(s_sheet_idle);
    place_sheet(0);
    show_guest_presets();
    paint_presets();
}

namespace {
void place_for_side()
{
    const Layout l = layout();
    lv_obj_set_x(s_rail, l.rail_x);
    lv_obj_set_pos(s_content, l.content_x, l.content_y);
    place_sheet(s_sheet_open ? DRAWER_W + GAP : 0);
    place_top_bar();
    place_notice();
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

}  // namespace ui::detail
