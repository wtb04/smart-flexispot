// The Setup page: how the screen looks, how the panel behaves, the focus
// timer's plan, and the screen and restarting, on the left; what the panel
// reports of itself on the right, in kit_front_setup_diag.cpp. Each list
// scrolls inside its own tile.

#include "kit_front.h"

#include "board.h"
#include "focus_model.h"
#include "kit_fonts.h"
#include "settings_model.h"
#include "topics.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <iterator>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;

constexpr std::int32_t COL_W = (inset::width - space::l) / 2;
constexpr std::int32_t RAIL_W = 250, PROGRESS_W = 170, SEG_2 = 230, SEG_3 = 300;

// The accents Setup has always offered, the first screen's default second.
constexpr std::uint32_t ACCENTS[] = {0xff6b5b, 0xff8738, 0xffb44a, 0xa8d75a, 0x5fcf7a,
                                     0x3fd0c0, 0x4fb0f5, 0x8b93ff, 0xb579f0, 0xff6fd0};
constexpr int          SWATCH_COLS = 5;
constexpr std::int32_t SWATCH = 88, SWATCH_GAP = space::l;
constexpr std::uint32_t TICK = 0x1f0e03;  // dark on every one of them

struct PlanSpec {
    const char *glyph, *title, *detail, *unit;
    int         min, max, step;
};
constexpr int      PLAN_COUNT = 4;
constexpr PlanSpec PLAN[PLAN_COUNT] = {
    {ic::timer, "Focus", nullptr, "min", 5, detail::FOCUS_WORK_MIN_MAX, 5},
    {ic::pause, "Break", nullptr, "min", 1, 30, 1},
    {ic::schedule, "Long break", nullptr, "min", 5, 60, 5},
    {ic::sync, "Rounds", "Before the long break", nullptr, 1, detail::FOCUS_ROUNDS_MAX, 1},
};

Rail     *s_brightness = nullptr, *s_volume = nullptr, *s_progress = nullptr;
lv_obj_t *s_brightness_row = nullptr, *s_volume_row = nullptr;
lv_obj_t *s_gate = nullptr, *s_charging = nullptr, *s_link = nullptr;
Stepper  *s_plan[PLAN_COUNT] = {};
lv_obj_t *s_restart = nullptr;
bool      s_update_ready = false;

lv_obj_t *setting_row(lv_obj_t *list, const char *glyph, const char *title, const char *detail = nullptr,
                      Trail trail = Trail::None)
{
    RowSpec spec;
    spec.glyph  = glyph;
    spec.title  = title;
    spec.detail = detail;
    spec.trail  = trail;
    return row(list, spec);
}

void percent_detail(lv_obj_t *row, int percent)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%d%%", percent);
    set_row_detail(row, buf);
}

Rail *slider(lv_obj_t *row, int min, int value)
{
    RailSpec spec;
    spec.length    = RAIL_W;
    spec.min       = min;
    spec.max       = 100;
    spec.draggable = true;
    auto *rail     = new Rail(row, spec);
    rail->set_value(value, false);
    lv_obj_set_scroll_chain(rail->obj(), false);  // a drag on it moves it, never the list under it
    return rail;
}

void choose(lv_obj_t *seg, int index)
{
    if (segmented_chosen(seg) != index) {
        segmented_choose(seg, index, true);
    }
}

void on_segment(lv_obj_t *seg, void (*picked)(int index))
{
    lv_obj_add_event_cb(
        seg,
        [](lv_event_t *e) {
            reinterpret_cast<void (*)(int)>(lv_event_get_user_data(e))(segmented_chosen(lv_event_get_current_target_obj(e)));
        },
        LV_EVENT_VALUE_CHANGED, reinterpret_cast<void *>(picked));
}

// --- appearance -----------------------------------------------------------

void pick_accent(lv_event_t *e)
{
    const auto colour = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(e)));
    set_accent(colour);
    if (detail::s_handlers.primary != nullptr) {
        detail::s_handlers.primary(colour);
    }
    close_popover(true);
}

// The look's own accent, until one is picked, is near one of them rather than one.
std::uint32_t nearest_accent()
{
    const std::uint32_t now  = colours().accent;
    const auto          away = [now](std::uint32_t c) {
        int d = 0;
        for (int shift : {16, 8, 0}) {
            d += std::abs(static_cast<int>((c >> shift) & 0xff) - static_cast<int>((now >> shift) & 0xff));
        }
        return d;
    };
    std::uint32_t best = ACCENTS[0];
    for (std::uint32_t c : ACCENTS) {
        best = away(c) < away(best) ? c : best;
    }
    return away(best) < 80 ? best : 0;
}

void open_accents(lv_obj_t *source)
{
    constexpr int          ROWS = (static_cast<int>(std::size(ACCENTS)) + SWATCH_COLS - 1) / SWATCH_COLS;
    constexpr std::int32_t GRID_W = SWATCH_COLS * SWATCH + (SWATCH_COLS - 1) * SWATCH_GAP;
    constexpr std::int32_t GRID_H = ROWS * SWATCH + (ROWS - 1) * SWATCH_GAP;
    // the sheet's padding, its head and the frame round its body
    constexpr std::int32_t W = GRID_W + 2 * space::xxl + 2 * space::s + 2 * space::m;
    constexpr std::int32_t H = GRID_H + 2 * space::xxl + size::button + space::l + 2 * space::s + 2 * space::m;
    lv_obj_t *body = sheet(source, "Accent colour", W, H);
    lv_obj_t *grid = flow(body, LV_FLEX_FLOW_ROW_WRAP, SWATCH_GAP);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(grid, SWATCH_GAP, 0);
    lv_obj_set_style_pad_all(grid, space::m, 0);
    lv_obj_set_style_pad_bottom(grid, space::m - space::s, 0);  // the body's own padding makes up the rest
    const std::uint32_t chosen = nearest_accent();
    for (std::uint32_t colour : ACCENTS) {
        lv_obj_t *swatch = lv_obj_create(grid);
        lv_obj_remove_style_all(swatch);
        lv_obj_set_size(swatch, SWATCH, SWATCH);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(swatch, radius::button, 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(colour), 0);
        lv_obj_set_style_bg_color(swatch, lv_color_darken(lv_color_hex(colour), 40), LV_STATE_PRESSED);
        lv_obj_add_event_cb(swatch, pick_accent, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::uintptr_t>(colour)));
        if (colour == chosen) {
            lv_obj_t *tick = icon(swatch, ic::check, 34);
            lv_obj_set_style_text_color(tick, lv_color_hex(TICK), 0);
            lv_obj_center(tick);
        }
    }
}

void build_appearance(lv_obj_t *list)
{
    section(list, "Appearance");
    s_brightness_row = setting_row(list, ic::light, "Brightness", "");
    s_brightness = slider(s_brightness_row, board::kMinBrightness, detail::s_initial_brightness);
    percent_detail(s_brightness_row, s_brightness->value());
    lv_obj_add_event_cb(
        s_brightness->obj(),
        [](lv_event_t *) {
            percent_detail(s_brightness_row, s_brightness->value());
            if (detail::s_handlers.brightness != nullptr) {
                detail::s_handlers.brightness(s_brightness->value());
            }
        },
        LV_EVENT_VALUE_CHANGED, nullptr);

    RowSpec accent;
    accent.glyph = ic::palette;
    accent.title = "Accent colour";
    accent.trail = Trail::Chevron;
    accent.dot   = true;
    accent.ink   = Ink::Accent;
    lv_obj_t *accent_row = row(list, accent);
    popover_source(accent_row);
    lv_obj_add_event_cb(accent_row, [](lv_event_t *e) { open_accents(lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *look = segmented(setting_row(list, ic::bedtime, "Look"), {"Dark", "Light"},
                               kit::look() == Look::Light ? 1 : 0, SEG_2);
    on_segment(look, [](int i) { set_look(i == 1 ? Look::Light : Look::Dark); });

    lv_obj_t *turn = segmented(setting_row(list, ic::aspect_ratio, "Orientation"),
                               {"Normal", "Flipped", "Auto"}, static_cast<int>(detail::s_orientation), SEG_3);
    on_segment(turn, [](int i) {
        const auto picked = static_cast<Orientation>(i);
        if (picked == detail::s_orientation) {
            return;
        }
        detail::s_orientation = picked;
        if (detail::s_handlers.orientation != nullptr) {
            detail::s_handlers.orientation(picked);
        }
    });
}

// --- behaviour ------------------------------------------------------------

void paint_settings()
{
    const detail::SettingsState &st = detail::settings_state();
    choose(s_gate, st.on[static_cast<int>(Setting::PresenceGate)] ? 1 : 0);
    choose(s_link, st.on[static_cast<int>(Setting::DeskBluetooth)] ? 1 : 0);
    lv_obj_t  *sw = row_trail(s_charging);
    const bool on = st.on[static_cast<int>(Setting::Charging)];
    if (lv_obj_has_state(sw, LV_STATE_CHECKED) != on) {
        lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    }
    const bool known = st.notification_volume >= 0;
    set_usable(s_volume->obj(), known);
    if (!known) {
        set_row_detail(s_volume_row, "--");
    } else if (!lv_obj_has_state(s_volume->obj(), LV_STATE_PRESSED)) {
        s_volume->set_value(st.notification_volume, false);
        percent_detail(s_volume_row, st.notification_volume);
    }
}

void build_behaviour(lv_obj_t *list)
{
    section(list, "Behaviour");
    s_gate = segmented(setting_row(list, ic::smartphone, "Pages while away", "Calendar and radar"), {"Show", "Hide"},
                       0, SEG_2);
    on_segment(s_gate, [](int i) { detail::settings_pick(Setting::PresenceGate, i == 1); });

    s_charging = setting_row(list, ic::bolt, "Battery charging", nullptr, Trail::Toggle);
    lv_obj_add_event_cb(
        row_trail(s_charging),
        [](lv_event_t *e) {
            detail::settings_pick(Setting::Charging, lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, nullptr);

    s_link = segmented(setting_row(list, ic::desk, "Desk link"), {"Wire", "Bluetooth"}, 0, SEG_2);
    on_segment(s_link, [](int i) { detail::settings_pick(Setting::DeskBluetooth, i == 1); });

    s_volume_row = setting_row(list, ic::volume_up, "Notification volume", "--");
    s_volume     = slider(s_volume_row, 0, 0);
    // a sound at the new level once it is let go
    for (lv_event_code_t code : {LV_EVENT_VALUE_CHANGED, LV_EVENT_RELEASED}) {
        lv_obj_add_event_cb(
            s_volume->obj(),
            [](lv_event_t *e) {
                percent_detail(s_volume_row, s_volume->value());
                if (detail::s_handlers.volume != nullptr) {
                    detail::s_handlers.volume(s_volume->value(), lv_event_get_code(e) == LV_EVENT_RELEASED);
                }
            },
            code, nullptr);
    }
    subscribe(Topic::Settings, detail::kNoView, paint_settings);
}

// --- the focus plan -------------------------------------------------------

void build_plan(lv_obj_t *list)
{
    section(list, "Focus plan");
    for (int i = 0; i < PLAN_COUNT; ++i) {
        const PlanSpec &p = PLAN[i];
        StepperSpec     spec;
        spec.min   = p.min;
        spec.max   = p.max;
        spec.step  = p.step;
        spec.value = p.min;
        spec.unit  = p.unit;
        s_plan[i]  = new Stepper(setting_row(list, p.glyph, p.title, p.detail), spec);
        lv_obj_add_event_cb(
            s_plan[i]->obj(),
            [](lv_event_t *) {
                if (detail::s_handlers.focus_plan != nullptr) {
                    detail::s_handlers.focus_plan(s_plan[0]->value(), s_plan[1]->value(), s_plan[2]->value(),
                                                  s_plan[3]->value());
                }
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
    }
    // one width, so their buttons line up in columns
    lv_obj_update_layout(list);
    std::int32_t widest = 0;
    for (Stepper *st : s_plan) {
        widest = std::max(widest, lv_obj_get_width(st->obj()));
    }
    for (Stepper *st : s_plan) {
        lv_obj_set_width(st->obj(), widest);
        lv_obj_set_flex_align(st->obj(), LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    subscribe(Topic::Focus, detail::kNoView, [] {
        const Focus &f = detail::focus_state();
        const int    values[PLAN_COUNT] = {f.work_min, f.break_min, f.long_break_min, f.rounds};
        for (int i = 0; i < PLAN_COUNT; ++i) {
            s_plan[i]->set_value(values[i]);
        }
    });
}

// --- the screen and restarting --------------------------------------------

// Restarting is what installs an update, so while one is ready the restart row offers it.
void paint_update()
{
    const detail::SettingsState &st = detail::settings_state();
    if (!st.update_known) {
        return;
    }
    const UpdateState &u     = st.update;
    const bool         busy  = u.busy != UpdateTarget::None;
    const bool         ready = u.panel_ready || u.companion_ready;
    s_update_ready           = ready && !busy;

    const char *title = busy ? (u.phase == UpdatePhase::Installing ? "Installing" : "Downloading")
                        : ready ? "Update now"
                                : "Restart";
    static const char *shown = nullptr;
    if (title != shown) {
        shown = title;
        set_row_title(s_restart, title, s_update_ready ? Ink::Accent : Ink::Primary);
        set_row_glyph(s_restart, busy ? ic::sync : ready ? ic::arrow_downward : ic::restart_alt);
    }
    char line[48];
    if (busy) {
        const char *who = u.busy == UpdateTarget::Panel ? "Panel" : "Companion";
        if (u.seconds_left >= 0) {
            std::snprintf(line, sizeof line, "%s, %d:%02d left", who, u.seconds_left / 60, u.seconds_left % 60);
        } else {
            std::snprintf(line, sizeof line, "%s, %d%%", who, u.percent);
        }
    } else {
        std::snprintf(line, sizeof line, "%s",
                      !ready                                 ? "Hold to restart"
                      : u.panel_ready && u.companion_ready   ? "Panel and companion ready"
                      : u.panel_ready                        ? "Panel ready"
                                                             : "Companion ready");
    }
    set_row_detail(s_restart, line);
    lv_obj_set_hidden(s_progress->obj(), !busy);
    if (busy && s_progress->value() != u.percent) {
        s_progress->set_value(u.percent, true);
    }
}

void build_power(lv_obj_t *column)
{
    lv_obj_t *g = group(column, COL_W);
    RowSpec   off;
    off.glyph  = ic::visibility_off;
    off.title  = "Screen off";
    off.detail = "Tap to wake";
    off.action = true;
    lv_obj_add_event_cb(row(g, off), [](lv_event_t *) { detail::set_screen_state(false); }, LV_EVENT_CLICKED,
                        nullptr);

    RowSpec restart;
    restart.glyph     = ic::restart_alt;
    restart.title     = "Restart";
    restart.detail    = "Hold to restart";
    restart.pressable = true;
    s_restart         = row(g, restart);
    RailSpec progress;
    progress.length = PROGRESS_W;
    progress.quiet  = true;
    s_progress      = new Rail(s_restart, progress);
    lv_obj_set_hidden(s_progress->obj(), true);
    lv_obj_add_event_cb(
        s_restart,
        [](lv_event_t *) {
            if (detail::s_handlers.restart != nullptr) {
                detail::s_handlers.restart();
            }
        },
        LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(
        s_restart,
        [](lv_event_t *) {
            if (s_update_ready && detail::s_handlers.update_now != nullptr) {
                detail::s_handlers.update_now();
            }
        },
        LV_EVENT_SHORT_CLICKED, nullptr);
    centre_rows(g);
    subscribe(Topic::Update, detail::kNoView, paint_update);
}
}  // namespace

void build_setup(lv_obj_t *page)
{
    lv_obj_t *cols = flow(page, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_size(cols, inset::width, inset::height);

    lv_obj_t *left = flow(cols, LV_FLEX_FLOW_COLUMN, space::l);
    lv_obj_set_size(left, COL_W, inset::height);
    lv_obj_t *list = kit::list(left, COL_W, 0);
    lv_obj_set_flex_grow(lv_obj_get_parent(list), 1);
    build_appearance(list);
    build_behaviour(list);
    build_plan(list);
    centre_rows(list);
    build_power(left);

    lv_obj_t *right = flow(cols, LV_FLEX_FLOW_COLUMN, space::l);
    lv_obj_set_size(right, COL_W, inset::height);
    build_setup_diagnostics(right, COL_W);
}
}  // namespace ui::kitfront
