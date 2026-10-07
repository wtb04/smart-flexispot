// What the panel reports of itself, on Setup's right: in one tile whether all
// is well, and each of its parts as a glyph in its state's ink, whose readings
// open as a sheet on a tap and whose log on a hold; and the log of them all.

#include "kit_front.h"

#include "diagnostics_model.h"
#include "kit_fonts.h"
#include "topics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;
using namespace kit;

constexpr int          GRID_COLS = 3;
constexpr std::int32_t STATE_DOT = 14, VALUE_W = 170;
constexpr std::int32_t CARD_W = 660, SHEET_ROW_H = 52, SHEET_MAX_H = 600;
// a sheet's padding, its head and the frame round its body
constexpr std::int32_t SHEET_CHROME = 2 * space::xxl + size::button + space::l + 3 * space::s;
constexpr std::int32_t LOG_W = 860, LOG_H = 600;
constexpr int          LOG_MAX = 200;
constexpr int          ALL     = -1;
constexpr std::int32_t END_SLACK = 8;  // near enough the end to count as there

lv_obj_t     *s_summary = nullptr, *s_summary_dot = nullptr;
lv_obj_t     *s_card[kMaxCards] = {};
lv_obj_t     *s_card_glyph[kMaxCards] = {};
Level         s_level[kMaxCards] = {};
std::uint32_t s_card_stamp[kMaxCards] = {};

// The sheet of one part's readings while it is open.
int           s_sheet = -1;
lv_obj_t     *s_sheet_state = nullptr;
lv_obj_t     *s_sheet_value[kMaxRows] = {};
std::uint32_t s_sheet_stamp[kMaxRows] = {};

struct Log : InPsram {
    lv_obj_t   *col = nullptr, *empty = nullptr, *warn = nullptr;
    lv_obj_t   *chips[kMaxCards + 1] = {};  // All, then each part
    lv_obj_t   *lines[LOG_MAX] = {};
    char        level[LOG_MAX] = {};
    lv_timer_t *timer = nullptr;
    int         card = ALL, count = -1;
    bool        warnings = false, redraw = true;
    char        last[kLogTextMax] = "";
};
Log *s_log = nullptr;

const char *glyph_of(Glyph glyph)
{
    switch (glyph) {
    case Glyph::Wifi: return ic::wifi;
    case Glyph::Home: return ic::home;
    case Glyph::Presence: return ic::smartphone;
    case Glyph::Desk: return ic::desk;
    case Glyph::Link: return ic::sync;
    case Glyph::Power: return ic::battery_full;
    case Glyph::Radar: return ic::flight;
    case Glyph::Calendar: return ic::calendar_month;
    case Glyph::System: return ic::settings;
    }
    return ic::info;
}

Ink ink_of(Level level, Ink neutral)
{
    switch (level) {
    case Level::Good: return Ink::Green;
    case Level::Warn: return Ink::Amber;
    case Level::Bad: return Ink::Red;
    default: return neutral;
    }
}

void set_text_if(lv_obj_t *label, const char *value)
{
    if (std::strcmp(lv_label_get_text(label), value) != 0) {
        lv_label_set_text(label, value);
    }
}

const char *or_dash(const char *value) { return value[0] != '\0' ? value : "--"; }

void paint_summary()
{
    int  poor = 0;
    bool bad  = false;
    for (int i = 0; i < detail::s_card_count; ++i) {
        poor += s_level[i] == Level::Warn || s_level[i] == Level::Bad ? 1 : 0;
        bad = bad || s_level[i] == Level::Bad;
    }
    static int shown = -1;
    const int  state = poor * 2 + (bad ? 1 : 0);
    if (state == shown) {
        return;
    }
    shown = state;
    char text[32];
    if (poor == 0) {
        std::snprintf(text, sizeof text, "All healthy");
    } else {
        std::snprintf(text, sizeof text, "%d need%s attention", poor, poor == 1 ? "s" : "");
    }
    const Ink ink = poor == 0 ? Ink::Primary : bad ? Ink::Red : Ink::Amber;
    set_text_if(s_summary, text);
    set_ink(s_summary, ink);
    set_dot(s_summary_dot, poor == 0 ? Ink::Green : ink);
}

// A part at a glance: as the text when well, in its ink when not, faint until told.
Ink glyph_ink(Level level) { return level == Level::Good ? Ink::Primary : ink_of(level, Ink::Tertiary); }

void paint_sheet()
{
    if (s_sheet < 0) {
        return;
    }
    const detail::InfoState &card = detail::diagnostics_state().cards[s_sheet];
    set_badge(s_sheet_state, or_dash(card.value), ink_of(card.level, Ink::Secondary));
    for (int r = 0; r < kMaxRows; ++r) {
        const detail::InfoState &info = detail::diagnostics_state().rows[s_sheet][r];
        if (s_sheet_value[r] != nullptr && info.stamp != s_sheet_stamp[r]) {
            s_sheet_stamp[r] = info.stamp;
            set_text_if(s_sheet_value[r], or_dash(info.value));
            set_ink(s_sheet_value[r], ink_of(info.level, Ink::Primary));
        }
    }
}

void paint()
{
    const detail::DiagnosticsState &d = detail::diagnostics_state();
    bool levels = false;
    for (int i = 0; i < detail::s_card_count; ++i) {
        const detail::InfoState &card = d.cards[i];
        if (card.stamp != s_card_stamp[i]) {
            s_card_stamp[i] = card.stamp;
            if (s_level[i] != card.level) {
                set_ink(s_card_glyph[i], glyph_ink(card.level));
                levels = true;
            }
            s_level[i] = card.level;
        }
    }
    if (levels) {
        paint_summary();
    }
    paint_sheet();
}

// --- a part's readings ----------------------------------------------------

void open_log(int card, lv_obj_t *source);

void open_card(int card, lv_obj_t *source)
{
    const Card        &c    = detail::s_cards[card];
    const int          rows = std::min(c.row_count, kMaxRows);
    const std::int32_t h    = std::min(SHEET_CHROME + std::max(rows, 1) * SHEET_ROW_H, SHEET_MAX_H);
    lv_obj_t *body = sheet(source, c.title, CARD_W, h);
    s_sheet = card;

    // in its head, the part's state and its log
    lv_obj_t *head = lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(body)), 0);
    lv_obj_set_flex_grow(lv_obj_get_child(head, 0), 1);
    s_sheet_state = badge(head, "--", Ink::Secondary, nullptr, VALUE_W, true);
    lv_obj_move_to_index(s_sheet_state, 1);
    lv_obj_t *log = button(head, Button::Raised, nullptr, "Log");
    lv_obj_set_style_pad_hor(log, space::xl, 0);
    lv_obj_move_to_index(log, 2);
    lv_obj_add_event_cb(
        log,
        [](lv_event_t *) {
            // out of the part it was opened from, once this tap is done: the sheet, and this button with it, goes as it opens
            lv_async_call(
                [](void *data) {
                    const int which = static_cast<int>(reinterpret_cast<std::intptr_t>(data));
                    open_log(which, s_card[which]);
                },
                reinterpret_cast<void *>(static_cast<std::intptr_t>(s_sheet)));
        },
        LV_EVENT_CLICKED, nullptr);

    for (int r = 0; r < kMaxRows; ++r) {
        s_sheet_value[r] = r < rows ? sheet_row(body, c.rows[r], "--") : nullptr;
        s_sheet_stamp[r] = 0;
    }
    if (rows == 0) {
        empty(body, ic::info, "Nothing to read");
    }
    lv_obj_add_event_cb(
        body,
        [](lv_event_t *) {
            s_sheet       = -1;
            s_sheet_state = nullptr;
            std::fill(std::begin(s_sheet_value), std::end(s_sheet_value), nullptr);
        },
        LV_EVENT_DELETE, nullptr);
    paint_sheet();
}

// --- the log --------------------------------------------------------------

Ink line_ink(char level)
{
    switch (level) {
    case 'E': return Ink::Red;
    case 'W': return Ink::Amber;
    case 'D': return Ink::Tertiary;
    default: return Ink::Secondary;
    }
}

LogLine *lines()
{
    // made once and kept: two hundred lines are 40 kB
    static auto *buf = static_cast<LogLine *>(lv_malloc(sizeof(LogLine) * LOG_MAX));
    return buf;
}

void refresh_log()
{
    Log *log = s_log;
    if (log == nullptr) {
        return;
    }
    LogLine  *buf   = lines();
    const int count = detail::s_handlers.log != nullptr && buf != nullptr
                          ? std::clamp(detail::s_handlers.log(log->card, log->warnings, buf, LOG_MAX), 0, LOG_MAX)
                          : 0;
    // a new line moves every label along one, so nothing new is nothing to do
    const char *last = count > 0 ? buf[count - 1].text : "";
    if (!log->redraw && count == log->count && std::strcmp(last, log->last) == 0) {
        return;
    }
    const bool at_end = log->redraw || lv_obj_get_scroll_bottom(log->col) <= END_SLACK;
    log->redraw = false;
    log->count  = count;
    std::snprintf(log->last, sizeof log->last, "%s", last);

    for (int i = 0; i < LOG_MAX; ++i) {
        lv_obj_t *&line = log->lines[i];
        if (i >= count) {
            if (line != nullptr) {
                lv_obj_set_hidden(line, true);
            }
            continue;
        }
        if (line == nullptr) {
            line = text(log->col, Role::Small, "", Ink::Secondary);
            lv_obj_set_width(line, LV_PCT(100));
            lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_WRAP);
            log->level[i] = 'I';
        }
        lv_obj_set_hidden(line, false);
        set_text_if(line, buf[i].text);
        if (log->level[i] != buf[i].level) {
            log->level[i] = buf[i].level;
            set_ink(line, line_ink(buf[i].level));
        }
    }
    lv_obj_set_hidden(log->empty, count > 0);
    if (at_end) {
        lv_obj_update_layout(log->col);
        lv_obj_scroll_to_y(log->col, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

void refilter()
{
    Log *log = s_log;
    for (int i = 0; i <= detail::s_card_count; ++i) {
        lv_obj_set_state(log->chips[i], LV_STATE_CHECKED, i - 1 == log->card);
    }
    lv_obj_set_state(log->warn, LV_STATE_CHECKED, log->warnings);
    log->redraw = true;
    refresh_log();
}

void open_log(int card, lv_obj_t *source)
{
    lv_obj_t *col = sheet(source, "Log", LOG_W, LOG_H);
    auto     *log = new Log{};
    log->col  = col;
    log->card = card;
    s_log     = log;

    // whose lines and which, between the title and the lines
    lv_obj_t *pop  = lv_obj_get_parent(lv_obj_get_parent(col));
    lv_obj_t *line = flow(pop, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_size(line, LV_PCT(100), size::button);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_move_to_index(line, 1);
    lv_obj_t *chips = flow(line, LV_FLEX_FLOW_ROW, space::s);
    lv_obj_set_size(chips, 0, size::button);
    lv_obj_set_flex_grow(chips, 1);
    lv_obj_set_scrollable(chips, true);
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i <= detail::s_card_count; ++i) {
        lv_obj_t *chip = button(chips, Button::Flat, nullptr, i == 0 ? "All" : detail::s_cards[i - 1].title, false, true);
        lv_obj_set_style_pad_hor(chip, space::l, 0);
        lv_obj_add_event_cb(
            chip,
            [](lv_event_t *e) {
                s_log->card = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
                refilter();
            },
            LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(i - 1)));
        log->chips[i] = chip;
    }
    log->warn = button(line, Button::Flat, ic::warning, "Warnings only", false, true);
    lv_obj_set_style_pad_hor(log->warn, space::l, 0);
    lv_obj_add_event_cb(
        log->warn,
        [](lv_event_t *) {
            s_log->warnings = !s_log->warnings;
            refilter();
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_set_style_pad_row(col, space::xs, 0);
    lv_obj_set_style_pad_hor(col, space::s, 0);
    log->empty = empty(col, ic::info, "Nothing logged yet");
    log->timer = lv_timer_create([](lv_timer_t *) { refresh_log(); }, 1000, nullptr);
    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            lv_timer_delete(s_log->timer);
            delete s_log;
            s_log = nullptr;
        },
        LV_EVENT_DELETE, nullptr);
    refilter();
    if (card >= 0) {
        lv_obj_update_layout(chips);
        lv_obj_scroll_to_view(log->chips[card + 1], LV_ANIM_OFF);
    }
}

// --- the summary ----------------------------------------------------------

// What a finger on a part shows: as a pressed row.
lv_style_t s_pressed;

void restyle_pressed(void *)
{
    lv_style_set_bg_color(&s_pressed, lv_color_hex(colours().raised_hi));
}

lv_obj_t *part(lv_obj_t *grid, int i, std::int32_t w, std::int32_t h)
{
    const Card &c    = detail::s_cards[i];
    lv_obj_t   *cell = flow(grid, LV_FLEX_FLOW_COLUMN, space::xs);
    lv_obj_set_size(cell, w, h);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(cell, radius::inner, 0);
    lv_obj_set_clickable(cell, true);
    lv_obj_add_style(cell, &s_pressed, LV_STATE_PRESSED);
    s_card_glyph[i] = icon(cell, glyph_of(c.glyph), 34, true, Ink::Tertiary);
    lv_obj_t *name  = text(cell, Role::Small, c.title, Ink::Secondary);
    lv_obj_set_size(name, w - 2 * space::s, fonts::small()->line_height);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

    popover_source(cell);
    const auto index = reinterpret_cast<void *>(static_cast<std::intptr_t>(i));
    lv_obj_add_event_cb(
        cell,
        [](lv_event_t *e) {
            open_card(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))),
                      lv_event_get_current_target_obj(e));
        },
        LV_EVENT_SHORT_CLICKED, index);
    lv_obj_add_event_cb(
        cell,
        [](lv_event_t *e) {
            open_log(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))),
                     lv_event_get_current_target_obj(e));
        },
        LV_EVENT_LONG_PRESSED, index);
    return cell;
}
}  // namespace

void build_setup_diagnostics(lv_obj_t *column, std::int32_t w, std::int32_t h)
{
    section(column, "Diagnostics");
    lv_obj_t *t = tile(column);
    lv_obj_set_size(t, w, h);
    lv_obj_set_style_pad_all(t, space::xl, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, space::s, 0);

    lv_obj_t *head = flow(t, LV_FLEX_FLOW_ROW, space::m);
    lv_obj_set_size(head, LV_PCT(100), size::button);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_summary_dot = dot(head, Ink::Green, STATE_DOT);
    s_summary     = text(head, Role::H1, "All healthy");
    lv_obj_set_width(s_summary, widest(Role::H1, {"All healthy", "9 need attention"}));
    filler(head);
    lv_obj_t *log = button(head, Button::Raised, nullptr, "Log");
    lv_obj_set_style_pad_hor(log, space::xl, 0);
    popover_source(log);
    lv_obj_add_event_cb(log, [](lv_event_t *e) { open_log(ALL, lv_event_get_current_target_obj(e)); },
                        LV_EVENT_CLICKED, nullptr);

    lv_style_init(&s_pressed);
    lv_style_set_bg_opa(&s_pressed, LV_OPA_COVER);
    restyle_pressed(nullptr);
    on_restyle(t, restyle_pressed, nullptr);

    const int count = std::min(detail::s_card_count, kMaxCards);
    if (count == 0) {
        empty(t, ic::monitor_heart, "Nothing reported");
    } else {
        lv_obj_t *grid = flow(t, LV_FLEX_FLOW_ROW_WRAP, 0);
        lv_obj_set_width(grid, LV_PCT(100));
        lv_obj_set_flex_grow(grid, 1);
        const int          lines  = (count + GRID_COLS - 1) / GRID_COLS;
        const std::int32_t cell_w = (w - 2 * space::xl) / GRID_COLS;
        const std::int32_t cell_h = (h - 2 * space::xl - size::button - space::s) / lines;
        for (int i = 0; i < count; ++i) {
            s_card[i] = part(grid, i, cell_w, cell_h);
        }
    }
    subscribe(Topic::Diagnostics, detail::kNoView, paint);
}
}  // namespace ui::kitfront
