#include "ui_internal.h"

#include "popout.h"
#include "room_model.h"
#include "status_model.h"
#include "topics.h"

#include "esp_timer.h"
#include "focus_model.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <utility>

// The dock at the panel's side: at its head the desk's height, which folds the
// rest of the desk out beside it, then Stand and Sit; the tabs stand up from
// its foot, Home lowest.
namespace ui::detail {
namespace {
constexpr std::int32_t DOCK_PAD      = 12;
constexpr std::int32_t DOCK_INNER_W  = DOCK_W - 2 * DOCK_PAD;  // Stand and Sit are as tall, square to the finger
constexpr std::int32_t DOCK_GAP      = 12;
constexpr std::int32_t DOCK_DESK_ICON = 46;
constexpr std::int32_t FOCUS_H     = 72;
constexpr std::int32_t FOCUS_APART = 16;  // from the tabs under it

constexpr std::int32_t SHEET_PAD     = 28;
constexpr std::int32_t SHEET_GAP     = 24;  // from the dock
constexpr std::int32_t SHEET_BTN_GAP = 20;
constexpr std::int32_t SHEET_INNER_W = DRAWER_W - 2 * SHEET_PAD;
constexpr std::int32_t SHEET_COLS    = 2;
constexpr std::int32_t SHEET_BTN_W   = (SHEET_INNER_W - (SHEET_COLS - 1) * SHEET_BTN_GAP) / SHEET_COLS;
constexpr std::int32_t SHEET_BTN_H   = 112;
// Folded out, it goes again once left alone this long.
constexpr std::uint32_t SHEET_IDLE_MS = 20 * 1000;

// The presets the dock has no room for.
constexpr int SHEET_PRESETS[] = {ULTRA_LOW_PRESET, 0, 4, 5};
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

lv_obj_t   *s_tabs          = nullptr;  // the dock's column of tabs
lv_obj_t   *s_focus_button  = nullptr;  // the timer, between the desk and the pages
lv_obj_t   *s_focus_mark    = nullptr;  // drawn while it is idle
lv_obj_t   *s_focus_left    = nullptr;  // how long is left while it runs
lv_obj_t   *s_dock_height   = nullptr;  // the height, at the dock's head
lv_obj_t   *s_dock_unit     = nullptr;  // cm, under it
lv_obj_t   *s_height_button = nullptr;  // around it: folds the desk out
Popout      s_desk;                     // the rest of the desk, folded out beside the height
lv_obj_t   *s_sheet         = nullptr;  // its card

void build_dock_desk(lv_obj_t *dock)
{
    lv_obj_t *head = lv_obj_create(dock);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, DOCK_INNER_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(head, DOCK_GAP, 0);
    lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_clickable(head, false);

    // As big as Stand and Sit under it, being as much a button.
    lv_obj_t *height = lv_button_create(head);
    s_height_button  = height;
    theme::style_button(height, theme::panel_light);
    lv_obj_set_size(height, DOCK_INNER_W, DOCK_INNER_W);
    theme::fill_accent(height, LV_STATE_CHECKED);
    lv_obj_set_flex_flow(height, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(height, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(height, [](lv_event_t *) { open_desk_sheet(!s_desk.open); }, LV_EVENT_CLICKED, nullptr);
    s_dock_height = theme::make_label(height, "--", theme::primary, fonts::size_28());
    s_dock_unit   = theme::make_label(height, "cm", theme::secondary, theme::type_label());

    add_shortcuts(head, true, DOCK_INNER_W, theme::radius::control, DOCK_DESK_ICON, theme::panel_light);
}

// The timer, idle as itself; running, how long is left in the part's colour,
// faded while paused. Away with the owner's pages while the phone is.
void paint_focus_button()
{
    const Focus &focus = focus_state();
    const bool   idle  = focus_idle(focus);
    lv_obj_set_hidden(s_focus_button, s_presence_gate && !status_state().present);
    lv_obj_set_hidden(s_focus_mark, !idle);
    lv_obj_set_hidden(s_focus_left, idle);
    if (idle) {
        return;
    }
    char text[16];
    focus_clock_text(focus, text, sizeof(text));
    theme::set_text(s_focus_left, text);
    const bool lit = lv_obj_has_state(s_focus_button, LV_STATE_CHECKED);
    theme::set_text_color(s_focus_left, lit ? theme::text : focus_ink(focus_resting(focus)));
    lv_obj_set_style_opa(s_focus_left, focus_paused(focus) ? LV_OPA_50 : LV_OPA_COVER, 0);
}

// Over the pages' tabs, a little apart from them, being no page itself.
void build_focus_button(lv_obj_t *tabs)
{
    s_focus_button = lv_button_create(tabs);
    theme::style_button(s_focus_button, theme::panel_light);
    lv_obj_set_size(s_focus_button, DOCK_INNER_W, FOCUS_H);
    lv_obj_set_style_margin_bottom(s_focus_button, FOCUS_APART, 0);
    theme::fill_accent(s_focus_button, LV_STATE_CHECKED);
    // On the accent while its card is out, as the height is, the timer on it in the text's colour.
    lv_obj_add_event_cb(s_focus_button, [](lv_event_t *) {
        toggle_focus_popout(s_focus_button, s_rail, [](bool open) {
            lv_obj_set_style_image_recolor(s_focus_mark, lv_color_hex(open ? theme::text : theme::secondary), 0);
            paint_focus_button();
        });
    }, LV_EVENT_CLICKED, nullptr);
    s_focus_mark = lv_image_create(s_focus_button);
    lv_image_set_src(s_focus_mark, &icons::timer_icon);
    lv_obj_set_style_image_recolor(s_focus_mark, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_image_recolor_opa(s_focus_mark, LV_OPA_COVER, 0);
    lv_obj_center(s_focus_mark);
    s_focus_left = theme::make_label(s_focus_button, "", theme::text, fonts::size_22());
    lv_obj_center(s_focus_left);
    for (lv_obj_t *part : {s_focus_mark, s_focus_left}) {
        lv_obj_set_clickable(part, false);
    }
    subscribe(Topic::Focus, kNoView, paint_focus_button);
    subscribe(Topic::Second, kNoView, paint_focus_button);
    subscribe(Topic::Status, kNoView, paint_focus_button);
}

void show_dock_height(int tenths)
{
    char text[12] = "--";
    if (tenths >= 0) {
        std::snprintf(text, sizeof(text), "%d.%d", tenths / 10, tenths % 10);
    }
    theme::set_text(s_dock_height, text);
}

// From the dock's edge, at the top, out across the page.
void place_sheet()
{
    const Layout l = layout();
    place_popout(s_desk, l.rail_right ? l.rail_x : l.rail_x + DOCK_W, 0, l.rail_right);
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

// As tall as the rows of buttons showing.
void fit_sheet()
{
    int showing = 0;
    for (const int index : SHEET_PRESETS) {
        showing += lv_obj_is_hidden(s_preset_buttons[index]) ? 0 : 1;
    }
    const int          rows = (showing + SHEET_COLS - 1) / SHEET_COLS + 1;
    lv_obj_set_height(s_sheet, 2 * SHEET_PAD + rows * SHEET_BTN_H + (rows - 1) * SHEET_BTN_GAP);
    fit_popout(s_desk);
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

void dock_tabs_done()
{
    build_focus_button(s_tabs);  // last in a column that stands up from the foot, so on top
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

    build_dock_desk(dock);
    s_tabs = lv_obj_create(dock);
    lv_obj_remove_style_all(s_tabs);
    lv_obj_set_size(s_tabs, DOCK_INNER_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_tabs, LV_FLEX_FLOW_COLUMN_REVERSE);
    lv_obj_set_style_pad_row(s_tabs, BUTTON_GAP / 2, 0);
    lv_obj_align(s_tabs, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_clickable(s_tabs, false);

    subscribe(Topic::Desk, kNoView, [] {
        paint_presets();
        paint_desk_shortcuts();
        static int s_height_shown = INT_MIN;
        const int  height         = desk_state().height_mm;
        if (std::exchange(s_height_shown, height) != height) {
            show_dock_height(height);
        }
    });
}

void open_desk_sheet(bool open)
{
    open_popout(s_desk, open);
}

void create_desk_sheet(lv_obj_t *parent)
{
    s_sheet       = build_popout(s_desk, parent, DRAWER_W, SHEET_GAP, GAP, SHEET_IDLE_MS);
    s_desk.button = s_height_button;
    s_desk.above  = s_rail;
    // On the accent while out, the height on it in the text's colour.
    s_desk.lit = [](bool open) {
        theme::set_text_color(s_dock_height, open ? theme::text : theme::primary);
        theme::set_text_color(s_dock_unit, open ? theme::text : theme::secondary);
    };
    lv_obj_set_style_pad_all(s_sheet, SHEET_PAD, 0);
    lv_obj_set_flex_flow(s_sheet, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_sheet, SHEET_BTN_GAP, 0);
    lv_obj_set_style_pad_column(s_sheet, SHEET_BTN_GAP, 0);

    // Up and down first, level with the height they change.
    create_move_button(s_sheet, LV_SYMBOL_UP, Move::Up, SHEET_BTN_W, SHEET_BTN_H);
    create_move_button(s_sheet, LV_SYMBOL_DOWN, Move::Down, SHEET_BTN_W, SHEET_BTN_H);
    for (const int index : SHEET_PRESETS) {
        add_sheet_preset(s_sheet, index);
    }

    place_sheet();
    show_guest_presets();
    paint_presets();
}

namespace {
void place_for_side()
{
    const Layout l = layout();
    lv_obj_set_x(s_rail, l.rail_x);
    lv_obj_set_pos(s_content, l.content_x, l.content_y);
    place_sheet();
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

namespace ui {
namespace {
constexpr std::int64_t BENCH_GIVE_UP_US = 2'000'000;

// The fold-out coming or going as it does, each frame drawn as soon as its
// animation has moved on.
int time_fold(bool open, char *out, std::size_t size)
{
    using namespace detail;
    open_desk_sheet(open);
    const std::int64_t began  = esp_timer_get_time();
    std::int64_t       drawn  = 0;
    std::int64_t       worst  = 0;
    int                frames = 0;
    while (popout_moving(s_desk) && esp_timer_get_time() - began < BENCH_GIVE_UP_US) {
        lv_anim_refr_now();
        const std::int64_t start = esp_timer_get_time();
        lv_refr_now(nullptr);
        const std::int64_t took = esp_timer_get_time() - start;
        drawn += took;
        worst = std::max(worst, took);
        ++frames;
    }
    const std::int64_t all = esp_timer_get_time() - began;
    return std::snprintf(out, size, "%-8s %2d frames in %3d ms, %4.1f fps; %5.1f ms a frame, %5.1f at most\n",
                         open ? "opening" : "closing", frames, static_cast<int>(all / 1000),
                         all > 0 ? frames * 1e6 / static_cast<double>(all) : 0.0,
                         frames > 0 ? drawn / 1000.0 / frames : 0.0, worst / 1000.0);
}
}  // namespace

int bench_sheet(char *out, std::size_t size)
{
    using namespace detail;
    if (s_sheet == nullptr) {
        return std::snprintf(out, size, "not built yet\n");
    }
    const int page = s_page;
    open_desk_sheet(false);
    while (popout_moving(s_desk)) {
        lv_anim_refr_now();
    }
    select_page(HOME_PAGE);
    lv_refr_now(nullptr);
    int n = time_fold(true, out, size);
    n += time_fold(false, out + n, size - n);
    select_page(page);
    return n;
}
}  // namespace ui
