#include "ui_internal.h"

#include "home_model.h"
#include "topics.h"

// The air along the top, and under it the radar's cards and what comes next.
namespace ui::detail {
namespace {
constexpr std::int32_t PILL_H       = 60;
constexpr std::int32_t PILL_PAD     = 22;
constexpr std::int32_t PILL_GAP     = 12;
constexpr std::int32_t PILL_DOT     = 14;
constexpr std::int32_t PILL_DOT_GAP = 10;

struct Pill {
    lv_obj_t *root   = nullptr;
    lv_obj_t *dot    = nullptr;
    lv_obj_t *column = nullptr;
    lv_obj_t *label  = nullptr;
    lv_obj_t *value  = nullptr;
    bool      shown  = false;
};
Pill         s_pills[kPillCount];
std::int32_t s_pill_row_w = 0;

// The pills showing share the row between them.
void reflow_pills()
{
    int visible = 0;
    for (const Pill &pill : s_pills) {
        visible += pill.shown ? 1 : 0;
    }
    if (visible == 0) {
        return;
    }
    const std::int32_t w    = (s_pill_row_w - (visible - 1) * PILL_GAP) / visible;
    const std::int32_t text = w - 2 * PILL_PAD - PILL_DOT - PILL_DOT_GAP;
    for (const Pill &pill : s_pills) {
        if (!pill.shown) {
            continue;
        }
        lv_obj_set_width(pill.root, w);
        lv_obj_set_width(pill.column, text);
        lv_obj_set_width(pill.label, text);
        lv_obj_set_width(pill.value, text);
    }
}

Pill make_pill(lv_obj_t *strip)
{
    lv_obj_t *pill = lv_obj_create(strip);
    lv_obj_set_size(pill, s_pill_row_w / kPillCount, PILL_H);
    theme::style_panel(pill, theme::panel_light, PILL_H / 2);
    lv_obj_set_style_pad_hor(pill, PILL_PAD, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, PILL_DOT_GAP, 0);

    lv_obj_t *dot = lv_obj_create(pill);
    lv_obj_set_size(dot, PILL_DOT, PILL_DOT);
    theme::style_panel(dot, theme::secondary, PILL_DOT / 2);
    lv_obj_set_clickable(dot, false);

    lv_obj_t *column = lv_obj_create(pill);
    lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(column, 1);
    theme::style_panel(column, theme::panel_light, 0);
    lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(column, false);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(column, -1, 0);

    lv_obj_t *label = theme::make_label(column, "", theme::secondary, fonts::size_16());
    lv_obj_t *value = theme::make_label(column, "", theme::text, fonts::size_22());
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_set_hidden(pill, true);
    return Pill{pill, dot, column, label, value, false};
}

void build_pills(lv_obj_t *parent, std::int32_t w)
{
    s_pill_row_w = w;

    lv_obj_t *strip = lv_obj_create(parent);
    lv_obj_set_pos(strip, 0, 0);
    lv_obj_set_size(strip, w, PILL_H);
    theme::style_panel(strip, theme::background, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(strip, PILL_GAP, 0);

    for (Pill &pill : s_pills) {
        pill = make_pill(strip);
    }
}
}  // namespace

std::uint32_t level_ink(Level level)
{
    switch (level) {
        case Level::Good: return theme::green;
        case Level::Warn: return theme::amber;
        case Level::Bad:  return theme::red;
        default:          return theme::secondary;
    }
}
namespace {
// The pills, as the home model has them.
void paint_home()
{
    const HomeState &home = home_state();
    for (int index = 0; index < kPillCount; ++index) {
        Pill            &pill  = s_pills[index];
        const PillState &state = home.pills[index];
        if (pill.root == nullptr) {
            continue;
        }
        const bool empty = state.label[0] == '\0';
        lv_obj_set_hidden(pill.root, empty);
        if (pill.shown == empty) {
            pill.shown = !empty;
            reflow_pills();
        }
        if (!empty) {
            theme::set_text(pill.label, state.label);
            theme::set_text(pill.value, state.value[0] != '\0' ? state.value : "--");
            theme::set_bg_color(pill.dot, level_ink(state.level));
        }
    }
}
}  // namespace

// The air along the top, and under it the radar as its page had it, the scope
// and the chosen aircraft's column, and what comes next beside them. What
// plays, the heating and the lights are the control bar's, so not here as well.
void build_home_page(lv_obj_t *page)
{
    const Layout l = layout();

    lv_obj_set_style_pad_all(page, PANEL_PAD, 0);
    const std::int32_t inner_w = l.content_w - 2 * PANEL_PAD;
    const std::int32_t inner_h = l.content_h - 2 * PANEL_PAD;

    build_pills(page, inner_w);

    const std::int32_t body_y  = PILL_H + BUTTON_GAP;
    const std::int32_t body_h  = inner_h - body_y;
    const std::int32_t radar_w = radar_cards_width(body_h);
    radar_home_area(page, 0, body_y, radar_w, body_h);
    build_next_tile(page, radar_w + BUTTON_GAP, body_y, inner_w - radar_w - BUTTON_GAP, body_h);

    subscribe(Topic::Home, kNoView, paint_home);
}

}  // namespace ui::detail
