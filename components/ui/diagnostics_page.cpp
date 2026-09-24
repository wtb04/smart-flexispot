#include "ui_internal.h"

namespace ui::detail {
lv_obj_t     *s_info[INFO_COUNT] = {};
namespace {
static_assert(INFO_CARD_COUNT == static_cast<int>(Subsystem::Count), "a tile per subsystem");
constexpr bool cards_in_order()
{
    for (int i = 0; i < INFO_CARD_COUNT; ++i) {
        if (static_cast<int>(INFO_CARDS[i].subsystem) != i) {
            return false;
        }
    }
    return true;
}
static_assert(cards_in_order(), "a tile is indexed by its subsystem");
constexpr std::int32_t LOG_W        = 800;
constexpr std::int32_t LOG_H        = 520;
constexpr std::int32_t DETAIL_W     = 660;
constexpr std::int32_t DETAIL_ROW_GAP = 2;
constexpr std::int32_t DETAIL_MAX_FRAC = 80;
constexpr std::int32_t DETAIL_ROW_H = 30;
constexpr std::int32_t TILE_DOT     = 14;
constexpr int LOG_LINE_MAX = 24;
}  // namespace

lv_obj_t *s_settings_view   = nullptr;
lv_obj_t *s_appearance_view = nullptr;
lv_obj_t *s_diag_view       = nullptr;
lv_obj_t *s_diag_summary    = nullptr;
lv_obj_t *s_volume_value    = nullptr;
lv_obj_t *s_volume_slider   = nullptr;

lv_obj_t *s_tile_value[INFO_CARD_COUNT] = {};
lv_obj_t *s_tile_dot[INFO_CARD_COUNT]   = {};
namespace {
lv_obj_t *s_detail[INFO_CARD_COUNT]     = {};
}  // namespace

Level     s_card_level[INFO_CARD_COUNT] = {};
namespace {
lv_obj_t *s_detail_title                = nullptr;
std::int32_t s_detail_height[INFO_CARD_COUNT] = {};
bool s_tile_long = false;

}  // namespace

int s_summary_card[INFO_COUNT] = {};
namespace {
std::optional<ModalOverlay> s_diagnostics;
std::optional<ModalOverlay> s_log_modal;
lv_obj_t                   *s_log_title = nullptr;
lv_obj_t                   *s_log_line[LOG_LINE_MAX] = {};
lv_obj_t                   *s_log_empty = nullptr;
lv_obj_t                   *s_log_pane  = nullptr;
int                         s_log_shown = -1;
lv_timer_t                 *s_log_timer = nullptr;
}  // namespace

std::uint32_t info_ink(Level level)
{
    switch (level) {
        case Level::Good: return theme::green;
        case Level::Warn: return theme::amber;
        case Level::Bad:  return theme::red;
        default:          return theme::text;
    }
}

void refresh_diag_summary()
{
    int poor = 0;
    for (Level level : s_card_level) {
        if (level == Level::Warn || level == Level::Bad) {
            ++poor;
        }
    }
    char text[48];
    if (poor == 0) {
        std::snprintf(text, sizeof(text), "%d subsystems, all healthy", INFO_CARD_COUNT);
    } else {
        std::snprintf(text, sizeof(text), "%d of %d need attention", poor, INFO_CARD_COUNT);
    }
    theme::set_text(s_diag_summary, text);
    theme::set_text_color(s_diag_summary, poor == 0 ? theme::secondary : theme::amber);
}
namespace {
std::uint32_t level_colour(char level)
{
    switch (level) {
        case 'E': return theme::red;
        case 'W': return theme::amber;
        case 'D': return theme::secondary;
        default: return theme::text;
    }
}

// Built on first sight rather than at startup: forty labels are a few kilobytes
// of the internal pool the Wi-Fi transport needs, and the log is rarely opened.
void refresh_log()
{
    if (s_log_shown < 0 || s_handlers.log == nullptr) {
        return;
    }
    auto *lines = static_cast<LogLine *>(heap_caps_malloc(
        sizeof(LogLine) * LOG_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (lines == nullptr) {
        return;
    }

    const int count = s_handlers.log(INFO_CARDS[s_log_shown].subsystem, lines, LOG_LINE_MAX);
    for (int i = 0; i < LOG_LINE_MAX; ++i) {
        lv_obj_set_hidden(s_log_line[i], i >= count);
        if (i < count) {
            theme::set_text(s_log_line[i], lines[i].text);
            theme::set_text_color(s_log_line[i], level_colour(lines[i].level));
        }
    }
    lv_obj_set_hidden(s_log_empty, count > 0);
    heap_caps_free(lines);
}

void log_tick(lv_timer_t *)
{
    if (s_log_modal.has_value() && s_log_modal->visible()) {
        refresh_log();
    }
}

void detail_clicked_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (!s_diagnostics.has_value() || std::exchange(s_tile_long, false)) {
        return;
    }
    for (int i = 0; i < INFO_CARD_COUNT; ++i) {
        lv_obj_set_hidden(s_detail[i], i != index);
    }
    theme::set_text(s_detail_title, INFO_CARDS[index].title);
    lv_obj_scroll_to_y(s_detail[index], 0, LV_ANIM_OFF);
    s_diagnostics->resize(DETAIL_W, s_detail_height[index]);
    s_diagnostics->open();
}

void log_held_cb(lv_event_t *e)
{
    const int index = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    if (!s_log_modal.has_value()) {
        return;
    }
    s_tile_long = true;
    s_log_shown = index;
    char title[48];
    std::snprintf(title, sizeof(title), "%s log", INFO_CARDS[index].title);
    theme::set_text(s_log_title, title);
    refresh_log();
    lv_obj_scroll_to_y(s_log_pane, 0, LV_ANIM_OFF);
    s_log_modal->open();
}
}  // namespace

void restart_held_cb(lv_event_t *)
{
    if (s_handlers.restart != nullptr) {
        s_handlers.restart();
    }
}

void volume_changed_cb(lv_event_t *e)
{
    auto     *slider  = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const int percent = static_cast<int>(lv_slider_get_value(slider));

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(s_volume_value, text);

    if (s_handlers.volume != nullptr) {
        s_handlers.volume(percent, lv_event_get_code(e) == LV_EVENT_RELEASED);
    }
}

// A tile names a reading and shows it large, as the diagnostics grid does. A
// tile that is a way somewhere or a thing to do is led by its title instead,
// with the explanation under it: the other way round, a description ends up the
// biggest thing on the card.
lv_obj_t *build_tile(lv_obj_t *parent, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h, const char *icon, const char *title, bool titled,
                     const lv_image_dsc_t *image)
{
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    theme::style_button(tile, theme::panel_light);
    lv_obj_set_style_pad_all(tile, 20, 0);

    lv_obj_t *glyph = nullptr;
    if (image != nullptr) {
        glyph = lv_image_create(tile);
        lv_image_set_src(glyph, image);
        lv_obj_set_style_image_recolor(glyph, lv_color_hex(theme::primary), 0);
        lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
        lv_obj_set_clickable(glyph, false);
    } else {
        glyph = theme::make_accent_label(tile, icon, fonts::size_28());
    }
    lv_obj_align(glyph, LV_ALIGN_TOP_LEFT, 0, 0);

    const lv_font_t *font = titled ? theme::type_title() : fonts::size_22();
    lv_obj_t *caption     = theme::make_label(tile, title,
                                              titled ? theme::text : theme::secondary, font);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 44, titled ? 0 : 4);
    lv_obj_set_width(caption, w - 96);
    lv_obj_set_height(caption, font->line_height);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS);
    return tile;
}

lv_obj_t *tile_note(lv_obj_t *tile, std::int32_t w, const char *initial)
{
    lv_obj_t *note = theme::make_label(tile, initial, theme::secondary, theme::type_body());
    lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_width(note, w - 40);
    lv_obj_set_height(note, theme::type_body()->line_height);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_DOTS);
    return note;
}
namespace {
lv_obj_t *tile_value(lv_obj_t *tile, std::int32_t w, const char *initial)
{
    lv_obj_t *value = theme::make_label(tile, initial, theme::text, fonts::size_32());
    lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_width(value, w - 40);
    lv_obj_set_height(value, fonts::size_32()->line_height);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    return value;
}

lv_obj_t *build_info_row(lv_obj_t *parent, const char *label, const lv_font_t *font,
                         std::int32_t height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    theme::style_panel(row, theme::panel, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(row, false);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    theme::make_label(row, label, theme::secondary, font);

    lv_obj_t *value = theme::make_label(row, "--", theme::text, font);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_height(value, font->line_height);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    return value;
}
}  // namespace

void build_info_tile(lv_obj_t *parent, int index, std::int32_t x, std::int32_t y, std::int32_t w,
                     std::int32_t h)
{
    lv_obj_t *tile =
        build_tile(parent, x, y, w, h, INFO_CARDS[index].icon, INFO_CARDS[index].title, false,
                   INFO_CARDS[index].subsystem == Subsystem::Radar      ? &icons::plane_icon
                   : INFO_CARDS[index].subsystem == Subsystem::Calendar ? &icons::calendar_icon
                                                                        : nullptr);
    lv_obj_add_event_cb(tile, detail_clicked_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_add_event_cb(tile, log_held_cb, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, TILE_DOT, TILE_DOT);
    theme::style_panel(dot, theme::secondary, TILE_DOT / 2);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_set_clickable(dot, false);

    s_tile_value[index] = tile_value(tile, w, "--");
    s_tile_dot[index]   = dot;
    s_summary_card[static_cast<int>(INFO_CARDS[index].summary)] = index;
}
namespace {
std::int32_t detail_height(const InfoCard &card)
{
    const std::int32_t body = card.count * DETAIL_ROW_H + (card.count - 1) * DETAIL_ROW_GAP;

    const Layout       l   = layout();
    const std::int32_t cap = std::min(l.screen_h * DETAIL_MAX_FRAC / 100, l.content_h - 2 * GAP);
    return std::min(2 * DETAIL_PAD + ModalOverlay::header_height() + HEADER_GAP + body, cap);
}
}  // namespace

void build_detail_overlay(lv_obj_t *parent)
{
    s_diagnostics.emplace(parent, DETAIL_W, detail_height(INFO_CARDS[0]));
    lv_obj_t *card = s_diagnostics->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    s_detail_title = theme::make_accent_label(card, "", fonts::size_28());
    lv_obj_align(s_detail_title, LV_ALIGN_TOP_LEFT, 0, 6);

    const std::int32_t width  = DETAIL_W - 2 * DETAIL_PAD;
    const std::int32_t body_y = ModalOverlay::header_height() + HEADER_GAP;

    for (int i = 0; i < INFO_CARD_COUNT; ++i) {
        s_detail_height[i] = detail_height(INFO_CARDS[i]);
        const std::int32_t height = s_detail_height[i] - 2 * DETAIL_PAD - body_y;
        lv_obj_t *panel = lv_obj_create(card);
        lv_obj_set_pos(panel, 0, body_y);
        lv_obj_set_size(panel, width, height);
        theme::style_panel(panel, theme::panel, 0);
        lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
        lv_obj_set_hidden(panel, true);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(panel, DETAIL_ROW_GAP, 0);
        lv_obj_set_scrollable(panel, true);
        lv_obj_set_scroll_dir(panel, LV_DIR_VER);

        for (int r = 0; r < INFO_CARDS[i].count; ++r) {
            const InfoRow &row = INFO_CARDS[i].rows[r];
            s_info[static_cast<int>(row.field)] =
                build_info_row(panel, row.label, fonts::size_20(), DETAIL_ROW_H);
        }

        s_detail[i] = panel;
    }

    s_diagnostics->add_close_button();
}

void build_log_overlay(lv_obj_t *parent)
{
    s_log_modal.emplace(parent, LOG_W, LOG_H);
    lv_obj_t *card = s_log_modal->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    s_log_title = theme::make_accent_label(card, "", fonts::size_28());
    lv_obj_align(s_log_title, LV_ALIGN_TOP_LEFT, 0, 6);

    const std::int32_t width  = LOG_W - 2 * DETAIL_PAD;
    const std::int32_t body_y = ModalOverlay::header_height() + HEADER_GAP;
    const std::int32_t height = LOG_H - 2 * DETAIL_PAD - body_y;

    lv_obj_t *pane = lv_obj_create(card);
    lv_obj_set_pos(pane, 0, body_y);
    lv_obj_set_size(pane, width, height);
    theme::style_panel(pane, theme::background, 12);
    lv_obj_set_style_pad_all(pane, 14, 0);
    lv_obj_set_scrollable(pane, true);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);

    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, 2, 0);
    s_log_pane = pane;

    for (int i = 0; i < LOG_LINE_MAX; ++i) {
        s_log_line[i] = theme::make_label(pane, "", theme::secondary, fonts::size_16());
        lv_obj_set_width(s_log_line[i], width - 28);
        lv_label_set_long_mode(s_log_line[i], LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_hidden(s_log_line[i], true);
    }
    s_log_empty = theme::make_label(pane, "Nothing logged yet", theme::secondary,
                                    fonts::size_16());

    s_log_modal->add_close_button();
    s_log_timer = lv_timer_create(log_tick, 1000, nullptr);
}

lv_obj_t *build_slider_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                            const char *title, int value, int low, lv_event_cb_t changed,
                            lv_obj_t **out_value)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_pos(root, 0, y);
    lv_obj_set_size(root, w, ROW_CARD_H);
    theme::style_panel(root, theme::panel_light, 14);
    lv_obj_set_style_pad_hor(root, 22, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(root, 18, 0);

    theme::make_accent_label(root, icon, fonts::size_28());

    lv_obj_t *caption = theme::make_label(root, title, theme::secondary, fonts::size_22());
    lv_obj_set_width(caption, 210);

    lv_obj_t *slider = lv_slider_create(root);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 18);
    lv_obj_set_ext_click_area(slider, 26);
    lv_slider_set_range(slider, low, 100);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme::panel), LV_PART_MAIN);
    theme::fill_accent(slider, LV_PART_INDICATOR);
    theme::fill_accent(slider, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 10, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, changed, LV_EVENT_RELEASED, nullptr);

    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", value);
    *out_value = theme::make_label(root, text, theme::text, fonts::size_22());
    lv_obj_set_width(*out_value, 64);
    lv_obj_set_style_text_align(*out_value, LV_TEXT_ALIGN_RIGHT, 0);
    return slider;
}

}  // namespace ui::detail
