#include "ui_internal.h"

#include "claude.h"
#include "popout.h"
#include "settings_model.h"
#include "topics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

// The laptops' Claude Code sessions: beside the lights while any has been
// heard from in the last ten minutes, or waits on you, a ring of one piece per
// session with how many there are inside it; under it a card of what each is
// doing. Gone the rest of the time, and while an update arrives, which needs
// its room.
namespace ui::detail {
namespace {
constexpr std::int32_t PILL_W       = TOP_H;  // round: the ring and its count, nothing more
constexpr std::int32_t RING         = 48;
constexpr std::int32_t RING_W       = 4;
constexpr int          RING_GAP_DEG = 14;  // between its pieces
constexpr int          FULL_TURN    = 360;
constexpr int          TOP_DEG      = 270;  // where LVGL's angles start, turned to the top
constexpr lv_opa_t     WORKS_ON_AMBER = LV_OPA_50;
constexpr lv_opa_t     RESTS_ON_AMBER = LV_OPA_20;

constexpr std::int32_t  CARD_W       = 720;
constexpr std::int32_t  CARD_PAD     = 20;
constexpr std::int32_t  ROW_GAP      = 8;
constexpr std::int32_t  ROW_H        = 76;
constexpr std::int32_t  ROW_PAD      = 18;
constexpr std::int32_t  ROW_INNER    = CARD_W - 2 * CARD_PAD;
constexpr std::int32_t  STATE_ICON   = 28;
constexpr std::int32_t  STEPS_W      = 140;
constexpr std::int32_t  STATE_W      = 120;
constexpr std::int32_t  TEXT_W       = ROW_INNER - 2 * ROW_PAD - STATE_ICON - STEPS_W - STATE_W - 4 * theme::space::m;
constexpr std::int32_t  NAME_GAP     = 10;  // between a session's project and its laptop
constexpr std::int32_t  STEP_BAR_H   = 6;
constexpr std::int32_t  DETAIL_INSET = ROW_PAD + STATE_ICON + theme::space::m;  // under the lines above it
constexpr std::int32_t  DETAIL_W     = (ROW_INNER - DETAIL_INSET - ROW_PAD - 2 * theme::space::l) / 2;
constexpr std::int32_t  WHEN_W       = 52;
constexpr std::int32_t  ACT_ICON     = 22;
constexpr std::int32_t  STEP_ICON    = 16;
constexpr int           LATELY_BESIDE_STEPS = claude::kStepCount;  // as many as the steps, to keep it level
constexpr int           ROWS         = 4;  // the most urgent; more, one opened, would run off the screen
constexpr std::uint32_t CARD_IDLE_MS = 30 * 1000;
constexpr std::int64_t  MS_PER_MIN   = 60 * 1000;
constexpr std::int64_t  MIN_PER_HOUR = 60;

struct Row {
    lv_obj_t *root    = nullptr;
    lv_obj_t *icon    = nullptr;
    lv_obj_t *project = nullptr;
    lv_obj_t *machine = nullptr;
    lv_obj_t *doing   = nullptr;
    lv_obj_t *steps   = nullptr;  // "step 5 of 8" and its bar, without a step list nothing
    lv_obj_t *steps_text = nullptr;
    lv_obj_t *steps_bar  = nullptr;
    lv_obj_t *state   = nullptr;
    lv_obj_t *age     = nullptr;
    lv_obj_t *detail  = nullptr;  // opened by a tap: its steps, and what it did last
    lv_obj_t *step_head = nullptr;
    lv_obj_t *step_line[claude::kStepCount]{};
    lv_obj_t *step_icon[claude::kStepCount]{};
    lv_obj_t *step_text[claude::kStepCount]{};
    lv_obj_t *steps_col  = nullptr;
    lv_obj_t *lately_col = nullptr;
    lv_obj_t *act_line[claude::kLatelyCount]{};
    lv_obj_t *act_when[claude::kLatelyCount]{};
    lv_obj_t *act_symbol[claude::kLatelyCount]{};  // LVGL's own, for editing and reading
    lv_obj_t *act_icon[claude::kLatelyCount]{};
    lv_obj_t *act_text[claude::kLatelyCount]{};
};

claude::Snapshot *s_snap = nullptr;  // in PSRAM, by LVGL's allocator: internal RAM is short
lv_obj_t         *s_slot = nullptr;
lv_obj_t         *s_ring[claude::kSessionCount]{};
lv_obj_t         *s_count = nullptr;
Popout            s_pop;
lv_obj_t         *s_summary = nullptr;
lv_obj_t         *s_more    = nullptr;
Row               s_rows[ROWS];
char              s_opened[sizeof(claude::Session::id)] = "";  // the session whose row is open

bool is_accent(claude::State state)
{
    return state == claude::State::Working;
}

std::uint32_t ink_of(claude::State state)
{
    switch (state) {
        case claude::State::NeedsYou: return theme::amber;
        case claude::State::Failed:   return theme::red;
        case claude::State::Done:     return theme::text;
        default:                      return theme::secondary;
    }
}

const lv_image_dsc_t *icon_of(claude::State state)
{
    switch (state) {
        case claude::State::NeedsYou: return &icons::session_asks_icon;
        case claude::State::Done:     return &icons::session_done_icon;
        case claude::State::Failed:   return &icons::session_failed_icon;
        default:                      return &icons::session_works_icon;
    }
}

const char *name_of(claude::State state)
{
    switch (state) {
        case claude::State::NeedsYou: return "Needs you";
        case claude::State::Working:  return "Working";
        case claude::State::Done:     return "Done";
        case claude::State::Failed:   return "Failed";
        default:                      return "Idle";
    }
}

void tint(lv_obj_t *image, bool accent, std::uint32_t colour)
{
    if (accent) {
        theme::tint_accent(image);
        return;
    }
    lv_obj_remove_style(image, &theme::accent_tint_style, 0);
    const lv_color_t next = lv_color_hex(colour);
    if (!theme::has_local_color(image, LV_STYLE_IMAGE_RECOLOR, 0, next)) {
        lv_obj_set_style_image_recolor(image, next, 0);
    }
}

std::int32_t width_of(const char *text, const lv_font_t *font)
{
    lv_point_t size{};
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

// "4 min", "2 h"; under a minute, "just now".
void write_age(char *out, std::size_t size, std::int64_t ms, bool ago)
{
    const std::int64_t min = ms / MS_PER_MIN;
    if (min < 1) {
        std::snprintf(out, size, "just now");
    } else if (min < MIN_PER_HOUR) {
        std::snprintf(out, size, ago ? "%d min ago" : "%d min", static_cast<int>(min));
    } else {
        std::snprintf(out, size, ago ? "%d h ago" : "%d h", static_cast<int>(min / MIN_PER_HOUR));
    }
}

void capitalise(char *text)
{
    if (text[0] >= 'a' && text[0] <= 'z') {
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    }
}

// ---- The pill ----

bool waits()
{
    return s_snap->count > 0 && s_snap->sessions[0].state == claude::State::NeedsYou;  // first, as it is sorted
}

// What the pill shows, to paint it again only when that changes.
struct PillLook {
    int           count = -1;
    claude::State states[claude::kSessionCount]{};

    bool operator==(const PillLook &) const = default;
};
PillLook s_painted;

void paint_pill()
{
    PillLook look;
    look.count = s_snap->count;
    for (int i = 0; i < std::min(s_snap->count, claude::kSessionCount); ++i) {
        look.states[i] = s_snap->sessions[i].state;
    }
    if (look == s_painted) {
        return;
    }
    s_painted        = look;
    const bool amber = waits();
    bool       busy  = false;
    for (int i = 0; i < s_snap->count; ++i) {
        busy = busy || s_snap->sessions[i].state == claude::State::Working;
    }
    theme::set_bg_color(s_slot, amber ? theme::amber : theme::panel);
    // Amber too while its card is out, or pressed: lit, the ring would go dark on dark.
    for (const lv_state_t state : {LV_STATE_PRESSED, LV_STATE_CHECKED}) {
        theme::set_bg_color(s_slot, amber ? theme::amber : theme::panel_light, state);
    }

    const int n    = std::min(s_snap->count, claude::kSessionCount);
    const int each = n > 0 ? FULL_TURN / n : FULL_TURN;
    const int gap  = n > 1 ? RING_GAP_DEG / 2 : 0;
    for (int i = 0; i < claude::kSessionCount; ++i) {
        lv_obj_t *piece = s_ring[i];
        lv_obj_set_hidden(piece, i >= n);
        if (i >= n) {
            continue;
        }
        lv_arc_set_bg_angles(piece, static_cast<lv_value_precise_t>(i * each + gap),
                             static_cast<lv_value_precise_t>((i + 1) * each - gap));
        const claude::State state = s_snap->sessions[i].state;
        if (amber) {
            theme::arc_accent_or(piece, false, theme::background, LV_PART_MAIN);
            lv_obj_set_style_arc_opa(piece,
                                     state == claude::State::NeedsYou ? static_cast<lv_opa_t>(LV_OPA_COVER)
                                     : is_accent(state)              ? WORKS_ON_AMBER
                                                                     : RESTS_ON_AMBER,
                                     LV_PART_MAIN);
        } else {
            theme::arc_accent_or(piece, is_accent(state), ink_of(state) == theme::text ? theme::secondary : ink_of(state),
                                 LV_PART_MAIN);
            lv_obj_set_style_arc_opa(piece, LV_OPA_COVER, LV_PART_MAIN);
        }
    }
    char count[8];
    std::snprintf(count, sizeof(count), "%d", s_snap->count);
    theme::set_text(s_count, count);
    theme::ink_accent_or(s_count, !amber && busy, amber ? theme::background : theme::secondary);
}

// ---- The card ----

void paint_detail(Row &row, const claude::Session &session)
{
    const bool steps = session.step_count > 0;
    lv_obj_set_hidden(row.steps_col, !steps);
    lv_obj_set_width(row.lately_col, steps ? DETAIL_W : 2 * DETAIL_W + 2 * theme::space::l);
    for (int i = 0; i < claude::kStepCount; ++i) {
        const bool shown = i < session.step_count;
        lv_obj_set_hidden(row.step_line[i], !shown);
        if (!shown) {
            continue;
        }
        const claude::StepItem &step = session.steps[i];
        const bool              now  = step.step == claude::Step::Now;
        lv_image_set_src(row.step_icon[i], step.step == claude::Step::Done ? &icons::step_done_icon
                                           : now                          ? &icons::step_now_icon
                                                                          : &icons::step_next_icon);
        tint(row.step_icon[i], now, step.step == claude::Step::Done ? theme::green : theme::disabled_ink);
        theme::set_text(row.step_text[i], step.text);
        theme::set_text_color(row.step_text[i], now ? theme::text : theme::secondary);
    }
    for (int i = 0; i < claude::kLatelyCount; ++i) {
        const bool shown = i < session.lately_count && (!steps || i < LATELY_BESIDE_STEPS);
        lv_obj_set_hidden(row.act_line[i], !shown);
        if (!shown) {
            continue;
        }
        const claude::Action &action = session.lately[i];
        char                  text[64];
        claude::describe(action, claude::Tense::Past, text, sizeof(text));
        theme::set_text(row.act_text[i], text);
        char when[16];
        write_age(when, sizeof(when), s_snap->now_ms - action.at_ms, false);
        theme::set_text(row.act_when[i], std::strcmp(when, "just now") == 0 ? "now" : when);
        const bool symbol = action.act == claude::Act::Edit || action.act == claude::Act::Read;
        lv_obj_set_hidden(row.act_symbol[i], !symbol);
        lv_obj_set_hidden(row.act_icon[i], symbol);
        if (symbol) {
            theme::set_text(row.act_symbol[i], action.act == claude::Act::Edit ? LV_SYMBOL_EDIT : LV_SYMBOL_FILE);
        } else {
            lv_image_set_src(row.act_icon[i], action.act == claude::Act::Search ? &icons::search_icon
                                              : action.act == claude::Act::Agents ? &icons::agents_icon
                                              : action.act == claude::Act::Web    ? &icons::web_icon
                                                                                  : &icons::run_icon);
        }
    }
}

void paint_row(Row &row, const claude::Session &session)
{
    const claude::State state = session.state;
    lv_image_set_src(row.icon, icon_of(state));
    tint(row.icon, is_accent(state), ink_of(state) == theme::text ? theme::secondary : ink_of(state));

    // The laptop just after the project's name, the name cut short where both would not fit.
    const lv_font_t   *name_font = fonts::size_22();
    const std::int32_t machine_w = width_of(session.machine, fonts::size_16());
    const std::int32_t name_w    = std::min(width_of(session.project, name_font) + 1,
                                            TEXT_W - machine_w - NAME_GAP);
    theme::set_text(row.project, session.project);
    lv_obj_set_width(row.project, std::max<std::int32_t>(name_w, 1));
    theme::set_text(row.machine, session.machine);
    lv_obj_set_x(row.machine, name_w + NAME_GAP);

    char doing[96] = "";
    switch (state) {
        case claude::State::NeedsYou:
            claude::describe(session.doing, claude::Tense::Wanted, doing, sizeof(doing));
            capitalise(doing);
            break;
        case claude::State::Working:
            claude::describe(session.doing, claude::Tense::Now, doing, sizeof(doing));
            if (doing[0] == '\0') {
                std::snprintf(doing, sizeof(doing), "Thinking");
            }
            if (session.agents > 0) {
                const std::size_t used = std::strlen(doing);
                std::snprintf(doing + used, sizeof(doing) - used, session.agents == 1 ? ", 1 agent" : ", %d agents",
                              session.agents);
            }
            break;
        case claude::State::Done:
            if (session.ran_ms >= MS_PER_MIN) {
                char ran[24];
                write_age(ran, sizeof(ran), session.ran_ms, false);
                std::snprintf(doing, sizeof(doing), "Finished after %s", ran);
            } else {
                std::snprintf(doing, sizeof(doing), "Finished");
            }
            break;
        case claude::State::Failed: std::snprintf(doing, sizeof(doing), "Stopped on an error"); break;
        default:                    std::snprintf(doing, sizeof(doing), "Waiting for a prompt"); break;
    }
    theme::set_text(row.doing, doing);
    theme::set_text_color(row.doing, state == claude::State::NeedsYou ? theme::amber : theme::secondary);

    // Hidden within its place, so the state beside it stays put; the line before
    // it takes the room.
    lv_obj_set_hidden(row.steps_text, session.steps_total <= 0);
    lv_obj_set_hidden(row.steps_bar, session.steps_total <= 0);
    lv_obj_set_width(row.doing, session.steps_total > 0 ? TEXT_W : TEXT_W + theme::space::m + STEPS_W);
    if (session.steps_total > 0) {
        char steps[32];
        std::snprintf(steps, sizeof(steps), "step %d of %d", std::min(session.steps_done + 1, session.steps_total),
                      session.steps_total);
        theme::set_text(row.steps_text, steps);
        lv_bar_set_range(row.steps_bar, 0, session.steps_total);
        lv_bar_set_value(row.steps_bar, session.steps_done, LV_ANIM_OFF);
        theme::fill_accent_or(row.steps_bar, is_accent(state),
                              state == claude::State::NeedsYou ? theme::amber : theme::secondary, LV_PART_INDICATOR);
    }

    theme::set_text(row.state, name_of(state));
    theme::ink_accent_or(row.state, is_accent(state), ink_of(state));
    char age[24];
    write_age(age, sizeof(age), s_snap->now_ms - session.since_ms, state == claude::State::Done);
    theme::set_text(row.age, age);

    const bool opened = std::strcmp(s_opened, session.id) == 0;
    lv_obj_set_hidden(row.detail, !opened);
    if (opened) {
        paint_detail(row, session);
    }
}

void paint_card()
{
    char summary[48];
    const int n = s_snap->count;
    if (s_snap->machines > 1) {
        std::snprintf(summary, sizeof(summary), "%d sessions on %d laptops", n, s_snap->machines);
    } else {
        std::snprintf(summary, sizeof(summary), n == 1 ? "1 session" : "%d sessions", n);
    }
    theme::set_text(s_summary, summary);
    for (int i = 0; i < ROWS; ++i) {
        lv_obj_set_hidden(s_rows[i].root, i >= n);
        if (i < n) {
            paint_row(s_rows[i], s_snap->sessions[i]);
        }
    }
    lv_obj_set_hidden(s_more, n <= ROWS);
    if (n > ROWS) {
        char more[64];
        std::snprintf(more, sizeof(more), "%d more, none of them waiting on you", n - ROWS);
        theme::set_text(s_more, more);
    }
    fit_popout(s_pop);
}

void refresh()
{
    claude::snapshot(*s_snap);
    const bool shown = s_snap->shown && settings_state().update.busy == UpdateTarget::None;
    lv_obj_set_hidden(s_slot, !shown);
    if (!shown) {
        if (s_pop.open) {
            open_popout(s_pop, false);
        }
        return;
    }
    paint_pill();
    if (s_pop.open) {
        paint_card();
    }
}

void row_tapped(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (index >= s_snap->count) {
        return;
    }
    const char *id = s_snap->sessions[index].id;
    if (std::strcmp(s_opened, id) == 0) {
        s_opened[0] = '\0';
    } else {
        std::snprintf(s_opened, sizeof(s_opened), "%s", id);
    }
    paint_card();
}

// Under the pill, from its edge towards the dock, unfolding away from it: the
// pill sits close to the status, with the room for the card the other way.
void pill_tapped(lv_event_t *)
{
    if (!s_pop.open) {
        lv_area_t at;
        lv_obj_get_coords(s_slot, &at);
        const bool right = layout().dock_right;
        place_popout(s_pop, right ? at.x2 + 1 + GAP : at.x1 - GAP, at.y2 + 1, right);
        paint_card();
    }
    open_popout(s_pop, !s_pop.open);
}

lv_obj_t *column(lv_obj_t *parent, std::int32_t w, std::int32_t gap)
{
    lv_obj_t *box = theme::make_box(parent);
    lv_obj_set_size(box, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, gap, 0);
    return box;
}

void build_detail(Row &row)
{
    row.detail = theme::make_box(row.root);
    lv_obj_set_size(row.detail, ROW_INNER, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(row.detail, DETAIL_INSET, 0);
    lv_obj_set_style_pad_right(row.detail, ROW_PAD, 0);
    lv_obj_set_style_pad_bottom(row.detail, theme::space::m, 0);
    lv_obj_set_flex_flow(row.detail, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row.detail, 2 * theme::space::l, 0);

    lv_obj_t *steps = column(row.detail, DETAIL_W, theme::space::s);
    row.steps_col   = steps;
    row.step_head   = theme::make_eyebrow(steps, "STEPS");
    for (int i = 0; i < claude::kStepCount; ++i) {
        row.step_line[i] = theme::make_box(steps);
        lv_obj_set_size(row.step_line[i], DETAIL_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row.step_line[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.step_line[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row.step_line[i], theme::space::s, 0);
        row.step_icon[i] = theme::make_icon(row.step_line[i], &icons::step_next_icon);
        row.step_text[i] = theme::make_line(row.step_line[i], theme::secondary, fonts::size_16(),
                                            DETAIL_W - STEP_ICON - theme::space::s);
    }

    // In two columns of its own when there are no steps beside it.
    lv_obj_t *lately = column(row.detail, DETAIL_W, theme::space::s);
    lv_obj_set_flex_flow(lately, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(lately, 2 * theme::space::l, 0);
    row.lately_col = lately;
    lv_obj_set_width(theme::make_eyebrow(lately, "LATELY"), lv_pct(100));
    for (int i = 0; i < claude::kLatelyCount; ++i) {
        row.act_line[i] = theme::make_box(lately);
        lv_obj_set_size(row.act_line[i], DETAIL_W, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row.act_line[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.act_line[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row.act_line[i], theme::space::s, 0);
        row.act_when[i] = theme::make_line(row.act_line[i], theme::secondary, fonts::size_16(), WHEN_W);
        lv_obj_t *mark  = theme::make_box(row.act_line[i]);
        lv_obj_set_size(mark, ACT_ICON, ACT_ICON);
        row.act_symbol[i] = theme::make_label(mark, LV_SYMBOL_EDIT, theme::secondary, fonts::size_16());
        lv_obj_center(row.act_symbol[i]);
        row.act_icon[i] = theme::make_icon(mark, &icons::run_icon, theme::secondary);
        lv_obj_center(row.act_icon[i]);
        row.act_text[i] = theme::make_line(row.act_line[i], theme::text, fonts::size_16(),
                                           DETAIL_W - WHEN_W - ACT_ICON - 2 * theme::space::s);
    }
    lv_obj_set_hidden(row.detail, true);
}

void build_row(Row &row, lv_obj_t *card, int index)
{
    row.root = lv_button_create(card);
    theme::style_button(row.root, theme::panel_light);
    lv_obj_set_size(row.root, ROW_INNER, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row.root, theme::radius::row, 0);
    lv_obj_set_style_pad_all(row.root, 0, 0);
    lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(row.root, false);
    lv_obj_add_event_cb(row.root, row_tapped, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    lv_obj_t *head = theme::make_box(row.root);
    lv_obj_set_size(head, ROW_INNER, ROW_H);
    lv_obj_set_style_pad_hor(head, ROW_PAD, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, theme::space::m, 0);

    row.icon = theme::make_icon(head, &icons::session_works_icon);

    lv_obj_t *lines = column(head, TEXT_W, theme::space::xs);
    lv_obj_set_overflow_visible(lines, true);  // its second line runs on over the steps when there are none
    lv_obj_t *names = theme::make_box(lines);
    lv_obj_set_size(names, TEXT_W, lv_font_get_line_height(fonts::size_22()));
    row.project = theme::make_line(names, theme::text, fonts::size_22(), TEXT_W);
    row.machine = theme::make_label(names, "", theme::secondary, fonts::size_16());
    lv_obj_set_clickable(row.machine, false);
    lv_obj_align(row.machine, LV_ALIGN_BOTTOM_LEFT, 0, -2);  // on the name's line, not its top
    row.doing = theme::make_line(lines, theme::secondary, fonts::size_16(), TEXT_W);

    row.steps      = column(head, STEPS_W, theme::space::s);
    row.steps_text = theme::make_line(row.steps, theme::secondary, fonts::size_16(), STEPS_W);
    row.steps_bar  = lv_bar_create(row.steps);
    lv_obj_set_size(row.steps_bar, STEPS_W, STEP_BAR_H);
    theme::style_panel(row.steps_bar, theme::panel, STEP_BAR_H / 2);
    lv_obj_set_style_radius(row.steps_bar, STEP_BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_clickable(row.steps_bar, false);

    lv_obj_t *state = column(head, STATE_W, theme::space::xs);
    lv_obj_set_style_flex_cross_place(state, LV_FLEX_ALIGN_END, 0);
    row.state = theme::make_line(state, theme::secondary, fonts::size_20(), STATE_W);
    row.age   = theme::make_line(state, theme::secondary, fonts::size_16(), STATE_W);
    lv_obj_set_style_text_align(row.state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_align(row.age, LV_TEXT_ALIGN_RIGHT, 0);

    build_detail(row);
}
}  // namespace

void build_claude_slot(lv_obj_t *bar)
{
    s_snap = new (lv_malloc(sizeof(claude::Snapshot))) claude::Snapshot{};

    s_slot = lv_button_create(bar);
    theme::style_button(s_slot, theme::panel);
    lv_obj_set_size(s_slot, PILL_W, TOP_H);
    lv_obj_set_style_radius(s_slot, theme::radius::pill, 0);
    lv_obj_set_ext_click_area(s_slot, BAR_GAP / 2);
    lv_obj_set_scrollable(s_slot, false);
    lv_obj_set_flex_flow(s_slot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_slot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_slot, 0, 0);
    theme::light_when_out(s_slot);
    lv_obj_add_event_cb(s_slot, pill_tapped, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *ring = theme::make_box(s_slot);
    lv_obj_set_size(ring, RING, RING);
    for (lv_obj_t *&piece : s_ring) {
        piece = lv_arc_create(ring);
        lv_obj_set_size(piece, RING, RING);
        lv_arc_set_rotation(piece, TOP_DEG);
        lv_obj_set_style_arc_width(piece, RING_W, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(piece, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(piece, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(piece, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(piece, 0, LV_PART_KNOB);
        lv_obj_set_clickable(piece, false);
        lv_obj_set_hidden(piece, true);
    }
    s_count = theme::make_label(ring, "0", theme::secondary, fonts::size_20());
    lv_obj_set_width(s_count, RING);
    lv_obj_set_style_text_align(s_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_clickable(s_count, false);
    theme::center_ink(s_count);  // the digits in the ring's middle, not the line
    lv_obj_set_hidden(s_slot, true);
}

void build_claude_card(lv_obj_t *screen, lv_obj_t *bar)
{
    lv_obj_t *card = build_popout(s_pop, screen, CARD_W, GAP, GAP, CARD_IDLE_MS);
    s_pop.button   = s_slot;
    s_pop.above    = bar;
    lv_obj_set_style_pad_all(card, CARD_PAD, 0);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, ROW_GAP, 0);

    lv_obj_t *head = theme::make_box(card);
    lv_obj_set_size(head, ROW_INNER, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(head, theme::space::xs, 0);
    lv_obj_set_style_pad_bottom(head, theme::space::xs, 0);
    lv_obj_t *title = theme::make_eyebrow(head, "CLAUDE CODE");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    s_summary = theme::make_label(head, "", theme::secondary, fonts::size_16());
    lv_obj_align(s_summary, LV_ALIGN_RIGHT_MID, 0, 0);

    for (int i = 0; i < ROWS; ++i) {
        build_row(s_rows[i], card, i);
    }
    s_more = theme::make_label(card, "", theme::secondary, fonts::size_16());
    lv_obj_set_style_pad_left(s_more, theme::space::xs, 0);

    subscribe(Topic::Claude, kNoView, refresh);
    subscribe(Topic::Update, kNoView, refresh);
    // The ages go on, and ten quiet minutes take the pill away.
    subscribe(Topic::Second, kNoView, [] {
        if (!lv_obj_is_hidden(s_slot)) {
            refresh();
        }
    });
}

}  // namespace ui::detail
