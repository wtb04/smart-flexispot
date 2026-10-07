// The Setup page: the screen and sound, which are changed now and then, on the
// left, with the focus timer's plan and the panel's own settings, set once, a
// tap away in sheets; how the panel is, and sleeping and restarting it, on the
// right, the first in kit_front_setup_diag.cpp.

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
constexpr std::int32_t RAIL_W = 230, PROGRESS_W = 170, SEG_2 = 230, SEG_3 = 300;
constexpr std::int32_t SHEET_W = 760;
constexpr std::int32_t SHEET_ROW = 80;
// a sheet's padding, its head and the frame round its body
constexpr std::int32_t SHEET_CHROME = 2 * space::xxl + size::button + space::l + 3 * space::s;

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
lv_obj_t *s_brightness_row = nullptr, *s_volume_row = nullptr, *s_plan_row = nullptr;
lv_obj_t *s_restart = nullptr;
bool      s_update_ready = false;

// What the sheets hold while one is open.
lv_obj_t *s_turn = nullptr, *s_gate = nullptr, *s_charging = nullptr, *s_link = nullptr;
Stepper  *s_plan[PLAN_COUNT] = {};

lv_obj_t *setting_row(lv_obj_t *parent, const char *glyph, const char *title, const char *detail = nullptr,
                      Trail trail = Trail::None)
{
    RowSpec spec;
    spec.glyph  = glyph;
    spec.title  = title;
    spec.detail = detail;
    spec.trail  = trail;
    lv_obj_t *r = row(parent, spec);
    row_height(r, SETUP_ROW_H);
    return r;
}

lv_obj_t *opening_row(lv_obj_t *parent, const char *glyph, const char *title, const char *detail,
                      void (*open)(lv_obj_t *source))
{
    lv_obj_t *r = setting_row(parent, glyph, title, detail, Trail::Chevron);
    popover_source(r);
    lv_obj_add_event_cb(
        r, [](lv_event_t *e) { reinterpret_cast<void (*)(lv_obj_t *)>(lv_event_get_user_data(e))(lv_event_get_current_target_obj(e)); },
        LV_EVENT_CLICKED, reinterpret_cast<void *>(open));
    return r;
}

lv_obj_t *settings_sheet(lv_obj_t *source, const char *title, int rows)
{
    return sheet(source, title, SHEET_W, SHEET_CHROME + rows * SHEET_ROW);
}

lv_obj_t *sheet_setting(lv_obj_t *body, const char *glyph, const char *title, const char *detail = nullptr,
                        Trail trail = Trail::None)
{
    lv_obj_t *r = setting_row(body, glyph, title, detail, trail);
    row_height(r, SHEET_ROW);
    return r;
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
    return rail;
}

void choose(lv_obj_t *seg, int index)
{
    if (seg != nullptr && segmented_chosen(seg) != index) {
        segmented_choose(seg, index, false);
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

// Whatever of a sheet was kept here is gone with it.
void forget_on_close(lv_obj_t *body, lv_obj_t **refs[], int count)
{
    auto **held = static_cast<lv_obj_t ***>(lv_malloc(sizeof(lv_obj_t **) * (count + 1)));
    std::copy(refs, refs + count, held);
    held[count] = nullptr;
    lv_obj_add_event_cb(
        body,
        [](lv_event_t *e) {
            auto **held = static_cast<lv_obj_t ***>(lv_event_get_user_data(e));
            for (lv_obj_t ***r = held; *r != nullptr; ++r) {
                **r = nullptr;
            }
            lv_free(held);
        },
        LV_EVENT_DELETE, held);
}

// --- the screen and sound -------------------------------------------------

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
    constexpr std::int32_t W = GRID_W + 2 * space::xxl + 2 * space::s + 2 * space::m;
    constexpr std::int32_t H = GRID_H + SHEET_CHROME + 2 * space::m - space::s;
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
        lv_obj_set_scrollable(swatch, false);
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

void paint_volume()
{
    const detail::SettingsState &st = detail::settings_state();
    const bool known = st.notification_volume >= 0;
    set_usable(s_volume->obj(), known);
    if (!known) {
        set_row_detail(s_volume_row, "--");
    } else if (!lv_obj_has_state(s_volume->obj(), LV_STATE_PRESSED)) {
        s_volume->set_value(st.notification_volume, false);
        percent_detail(s_volume_row, st.notification_volume);
    }
}

void build_screen(lv_obj_t *column)
{
    section(column, "Screen and sound");
    lv_obj_t *g = group(column, COL_W);

    s_brightness_row = setting_row(g, ic::light, "Brightness", "");
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

    s_volume_row = setting_row(g, ic::volume_up, "Notification volume", "--");
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

    lv_obj_t *look = segmented(setting_row(g, ic::bedtime, "Look"), {"Dark", "Light"},
                               kit::look() == Look::Light ? 1 : 0, SEG_2);
    on_segment(look, [](int i) { detail::settings_pick(Setting::Light, i == 1); });

    RowSpec accent;
    accent.glyph = ic::palette;
    accent.title = "Accent colour";
    accent.trail = Trail::Chevron;
    accent.dot   = true;
    accent.ink   = Ink::Accent;
    lv_obj_t *accent_row = row(g, accent);
    row_height(accent_row, SETUP_ROW_H);
    popover_source(accent_row);
    lv_obj_add_event_cb(accent_row, [](lv_event_t *e) { open_accents(lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);

    subscribe(Topic::Settings, detail::kNoView, [look] {
        choose(look, detail::settings_state().on[static_cast<int>(Setting::Light)] ? 1 : 0);
        paint_volume();
    });
}

// --- the panel's own settings, set once -----------------------------------

void paint_panel()
{
    const detail::SettingsState &st = detail::settings_state();
    choose(s_gate, st.on[static_cast<int>(Setting::PresenceGate)] ? 1 : 0);
    choose(s_link, st.on[static_cast<int>(Setting::DeskBluetooth)] ? 1 : 0);
    choose(s_turn, static_cast<int>(detail::s_orientation));
    if (s_charging != nullptr) {
        lv_obj_t  *sw = row_trail(s_charging);
        const bool on = st.on[static_cast<int>(Setting::Charging)];
        if (lv_obj_has_state(sw, LV_STATE_CHECKED) != on) {
            lv_obj_set_state(sw, LV_STATE_CHECKED, on);
        }
    }
}

void open_panel(lv_obj_t *source)
{
    lv_obj_t *body = settings_sheet(source, "Panel", 4);

    s_turn = segmented(sheet_setting(body, ic::aspect_ratio, "Orientation"), {"Normal", "Flipped", "Auto"},
                       static_cast<int>(detail::s_orientation), SEG_3);
    on_segment(s_turn, [](int i) {
        const auto picked = static_cast<Orientation>(i);
        if (picked == detail::s_orientation) {
            return;
        }
        detail::s_orientation = picked;
        if (detail::s_handlers.orientation != nullptr) {
            detail::s_handlers.orientation(picked);
        }
    });

    s_gate = segmented(sheet_setting(body, ic::smartphone, "Pages while away", "Calendar and radar"),
                       {"Show", "Hide"}, 0, SEG_2);
    on_segment(s_gate, [](int i) { detail::settings_pick(Setting::PresenceGate, i == 1); });

    s_charging = sheet_setting(body, ic::bolt, "Battery charging", nullptr, Trail::Toggle);
    lv_obj_add_event_cb(
        row_trail(s_charging),
        [](lv_event_t *e) {
            detail::settings_pick(Setting::Charging, lv_obj_has_state(lv_event_get_current_target_obj(e), LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, nullptr);

    s_link = segmented(sheet_setting(body, ic::desk, "Desk link"), {"Wire", "Bluetooth"}, 0, SEG_2);
    on_segment(s_link, [](int i) { detail::settings_pick(Setting::DeskBluetooth, i == 1); });

    lv_obj_t **refs[] = {&s_turn, &s_gate, &s_charging, &s_link};
    forget_on_close(body, refs, static_cast<int>(std::size(refs)));
    paint_panel();
}

// --- the focus plan -------------------------------------------------------

void paint_plan()
{
    const Focus &f = detail::focus_state();
    char         line[64];
    std::snprintf(line, sizeof line, "%d min focus, %d min break, %d rounds", f.work_min, f.break_min, f.rounds);
    set_row_detail(s_plan_row, line);
    const int values[PLAN_COUNT] = {f.work_min, f.break_min, f.long_break_min, f.rounds};
    for (int i = 0; i < PLAN_COUNT; ++i) {
        if (s_plan[i] != nullptr) {
            s_plan[i]->set_value(values[i]);
        }
    }
}

void open_plan(lv_obj_t *source)
{
    lv_obj_t *body = settings_sheet(source, "Focus plan", PLAN_COUNT);
    for (int i = 0; i < PLAN_COUNT; ++i) {
        const PlanSpec &p = PLAN[i];
        StepperSpec     spec;
        spec.min   = p.min;
        spec.max   = p.max;
        spec.step  = p.step;
        spec.value = p.min;
        spec.unit  = p.unit;
        s_plan[i]  = new Stepper(sheet_setting(body, p.glyph, p.title, p.detail), spec);
        lv_obj_add_event_cb(
            s_plan[i]->obj(),
            [](lv_event_t *) {
                if (detail::s_handlers.focus_plan != nullptr) {
                    detail::s_handlers.focus_plan(s_plan[0]->value(), s_plan[1]->value(), s_plan[2]->value(),
                                                  s_plan[3]->value());
                }
            },
            LV_EVENT_VALUE_CHANGED, nullptr);
        // after the stepper's own, which stops its repeat
        lv_obj_add_event_cb(
            s_plan[i]->obj(),
            [](lv_event_t *e) {
                auto *st = static_cast<Stepper *>(lv_event_get_user_data(e));
                for (Stepper *&held : s_plan) {
                    held = held == st ? nullptr : held;
                }
                delete st;
            },
            LV_EVENT_DELETE, s_plan[i]);
    }
    // one width, so their buttons line up in columns
    lv_obj_update_layout(body);
    std::int32_t widest = 0;
    for (Stepper *st : s_plan) {
        widest = std::max(widest, lv_obj_get_width(st->obj()));
    }
    for (Stepper *st : s_plan) {
        lv_obj_set_width(st->obj(), widest);
        lv_obj_set_flex_align(st->obj(), LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    paint_plan();
}

void build_more(lv_obj_t *column)
{
    lv_obj_t *head = section(column, "Focus and panel");
    lv_obj_set_style_margin_top(head, space::l, 0);
    lv_obj_t *g = group(column, COL_W);
    s_plan_row  = opening_row(g, ic::timer, "Focus plan", "", open_plan);
    opening_row(g, ic::tune, "Panel", "Orientation, charging, desk link and more", open_panel);
    subscribe(Topic::Focus, detail::kNoView, paint_plan);
    subscribe(Topic::Settings, detail::kNoView, paint_panel);
}

// --- sleeping and restarting ----------------------------------------------

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
    lv_obj_t *head = section(column, "Sleep and restart");
    lv_obj_set_style_margin_top(head, space::l, 0);
    lv_obj_t *g = group(column, COL_W);
    RowSpec   off;
    off.glyph  = ic::visibility_off;
    off.title  = "Screen off";
    off.detail = "Tap to wake";
    off.action = true;
    lv_obj_t *off_row = row(g, off);
    row_height(off_row, SETUP_ROW_H);
    lv_obj_add_event_cb(off_row, [](lv_event_t *) { detail::set_screen_state(false); }, LV_EVENT_CLICKED, nullptr);

    RowSpec restart;
    restart.glyph     = ic::restart_alt;
    restart.title     = "Restart";
    restart.detail    = "Hold to restart";
    restart.pressable = true;
    s_restart         = row(g, restart);
    row_height(s_restart, SETUP_ROW_H);
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
    subscribe(Topic::Update, detail::kNoView, paint_update);
}
}  // namespace

void build_setup(lv_obj_t *page)
{
    lv_obj_t *cols = flow(page, LV_FLEX_FLOW_ROW, space::l);
    lv_obj_set_size(cols, inset::width, inset::height);

    lv_obj_t *left = flow(cols, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_size(left, COL_W, inset::height);
    build_screen(left);
    build_more(left);

    lv_obj_t *right = flow(cols, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_size(right, COL_W, inset::height);
    build_setup_diagnostics(right, COL_W, 4 * SETUP_ROW_H);
    build_power(right);
}
}  // namespace ui::kitfront
