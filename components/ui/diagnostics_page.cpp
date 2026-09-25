#include "ui_internal.h"

namespace ui::detail {
const Card *s_cards      = nullptr;
int         s_card_count = 0;
lv_obj_t   *s_row_value[kMaxCards][kMaxRows] = {};
namespace {
const char *glyph_symbol(Glyph glyph)
{
    switch (glyph) {
        case Glyph::Wifi:     return LV_SYMBOL_WIFI;
        case Glyph::Home:     return LV_SYMBOL_HOME;
        case Glyph::Presence: return LV_SYMBOL_EYE_OPEN;
        case Glyph::Desk:     return LV_SYMBOL_UP;
        case Glyph::Link:     return LV_SYMBOL_SHUFFLE;
        case Glyph::Power:    return LV_SYMBOL_BATTERY_FULL;
        case Glyph::System:   return LV_SYMBOL_SETTINGS;
        default:              return "";
    }
}

const lv_image_dsc_t *glyph_image(Glyph glyph)
{
    return glyph == Glyph::Radar      ? &icons::plane_icon
           : glyph == Glyph::Calendar ? &icons::calendar_icon
                                      : nullptr;
}

constexpr int WHOLE_PERCENT = 100;

constexpr std::int32_t  LOG_W           = 860;
constexpr std::int32_t  LOG_H           = 560;
constexpr std::int32_t  CHIP_H          = 48;
constexpr std::int32_t  CHIP_GAP        = 8;
constexpr std::int32_t  CHIP_PAD_X      = 18;
constexpr std::int32_t  WARN_W          = 200;
constexpr std::int32_t  WARN_Y          = -8;  // level with the close button
constexpr std::int32_t  LOG_GAP         = 20;  // between the chips and the lines
constexpr std::int32_t  LOG_PANE_RADIUS = 12;
constexpr std::int32_t  LOG_PANE_PAD    = 14;
constexpr std::int32_t  LOG_LINE_GAP    = 2;
constexpr std::int32_t  LOG_END_SLACK   = 8;   // near enough the bottom to count as there
constexpr std::uint32_t LOG_REFRESH_MS  = units::kMsPerSecond;
constexpr int           LOG_LINE_MAX    = 200;

// What the log is showing besides one card's lines.
constexpr int LOG_ALL_CARDS = -1;
constexpr int LOG_CLOSED    = -2;

constexpr std::int32_t DETAIL_W               = 660;
constexpr std::int32_t DETAIL_ROW_GAP         = 2;
constexpr std::int32_t DETAIL_MAX_PERCENT     = 80;  // of the screen's height
constexpr std::int32_t DETAIL_ROW_H           = 30;
constexpr std::int32_t DETAIL_H_WITHOUT_CARDS = 200;
constexpr std::int32_t INFO_ROW_GAP           = 12;

constexpr std::int32_t TILE_PAD          = 20;
constexpr std::int32_t TILE_DOT          = 14;
constexpr std::int32_t TILE_DOT_Y        = 8;
constexpr std::int32_t CAPTION_X         = 44;  // past the glyph
constexpr std::int32_t CAPTION_READING_Y = 4;
constexpr std::int32_t CAPTION_END_GAP   = 12;

constexpr std::int32_t SLIDER_CAPTION_W    = 210;
constexpr std::int32_t SLIDER_H            = 18;
constexpr std::int32_t SLIDER_TOUCH_MARGIN = 26;
constexpr std::int32_t SLIDER_KNOB_PAD     = 10;
constexpr std::int32_t SLIDER_VALUE_W      = 64;
}  // namespace

lv_obj_t *s_settings_view   = nullptr;
lv_obj_t *s_appearance_view = nullptr;
lv_obj_t *s_diag_view       = nullptr;
lv_obj_t *s_diag_summary    = nullptr;
lv_obj_t *s_volume_value    = nullptr;
lv_obj_t *s_volume_slider   = nullptr;

lv_obj_t *s_tile_value[kMaxCards] = {};
lv_obj_t *s_tile_dot[kMaxCards]   = {};
namespace {
lv_obj_t *s_detail[kMaxCards]     = {};
}  // namespace

Level     s_card_level[kMaxCards] = {};
namespace {
lv_obj_t *s_detail_title                = nullptr;
std::int32_t s_detail_height[kMaxCards] = {};
bool s_tile_long = false;

}  // namespace

namespace {
std::optional<ModalOverlay> s_diagnostics;
std::optional<ModalOverlay> s_log_modal;
lv_obj_t                   *s_log_line[LOG_LINE_MAX] = {};
lv_obj_t                   *s_log_empty = nullptr;
lv_obj_t                   *s_log_pane  = nullptr;
int                         s_log_shown = LOG_CLOSED;
bool                        s_log_warnings = false;
bool                        s_log_redraw   = false;  // the filter changed, so draw whatever comes
lv_obj_t                   *s_log_chip[kMaxCards + 1] = {};  // All, then each card
lv_obj_t                   *s_log_warn = nullptr;

int chip_card(int chip)
{
    return chip - 1;
}
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
    for (int i = 0; i < s_card_count; ++i) {
        if (s_card_level[i] == Level::Warn || s_card_level[i] == Level::Bad) {
            ++poor;
        }
    }
    char text[48];
    if (poor == 0) {
        std::snprintf(text, sizeof(text), "All healthy");
    } else {
        std::snprintf(text, sizeof(text), "%d need%s attention", poor, poor == 1 ? "s" : "");
    }
    theme::set_text(s_diag_summary, text);
    theme::set_text_color(s_diag_summary, poor == 0 ? theme::secondary : theme::amber);
}

void write_percent(lv_obj_t *label, int percent)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    theme::set_text(label, text);
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

void scroll_log_to_end()
{
    lv_obj_scroll_to_y(s_log_pane, LV_COORD_MAX, LV_ANIM_OFF);
}

void refresh_log()
{
    if (s_log_shown == LOG_CLOSED || s_handlers.log == nullptr) {
        return;
    }
    // Kept rather than asked for every second: two hundred lines are 40 KB.
    static LogLine *lines = static_cast<LogLine *>(heap_caps_malloc(
        sizeof(LogLine) * LOG_LINE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (lines == nullptr) {
        return;
    }

    // Stay at the newest line unless someone has scrolled up to read.
    const bool at_end = lv_obj_get_scroll_bottom(s_log_pane) <= LOG_END_SLACK;
    const int  count  = s_handlers.log(s_log_shown, s_log_warnings, lines, LOG_LINE_MAX);

    // A new line moves every label along one, so nothing new is nothing to do.
    static int  shown_count = -1;
    static char shown_last[kLogTextMax];
    const char *last = count > 0 ? lines[count - 1].text : "";
    if (!s_log_redraw && count == shown_count && std::strcmp(last, shown_last) == 0) {
        return;
    }
    s_log_redraw = false;
    shown_count  = count;
    std::snprintf(shown_last, sizeof(shown_last), "%s", last);

    for (int i = 0; i < LOG_LINE_MAX; ++i) {
        lv_obj_set_hidden(s_log_line[i], i >= count);
        if (i < count) {
            theme::set_text(s_log_line[i], lines[i].text);
            theme::set_text_color(s_log_line[i], level_colour(lines[i].level));
        }
    }
    lv_obj_set_hidden(s_log_empty, count > 0);
    if (at_end) {
        lv_obj_update_layout(s_log_pane);
        scroll_log_to_end();
    }
}

void paint_log_filter()
{
    for (int i = 0; i <= s_card_count; ++i) {
        const bool picked = chip_card(i) == s_log_shown;
        lv_obj_set_state(s_log_chip[i], LV_STATE_CHECKED, picked);
        theme::set_text_color(lv_obj_get_child(s_log_chip[i], 0),
                              picked ? theme::text : theme::secondary);
    }
    lv_obj_set_state(s_log_warn, LV_STATE_CHECKED, s_log_warnings);
    theme::set_text_color(lv_obj_get_child(s_log_warn, 0),
                          s_log_warnings ? theme::text : theme::secondary);
}

/** Draws the log again from its end, after what it is filtered by changed. */
void refilter_log()
{
    s_log_redraw = true;
    paint_log_filter();
    scroll_log_to_end();
    refresh_log();
}

void open_log(int card)
{
    if (!s_log_modal.has_value()) {
        return;
    }
    s_log_shown = card;
    refilter_log();
    lv_obj_update_layout(s_log_pane);
    scroll_log_to_end();
    s_log_modal->open();
}

void log_chip_cb(lv_event_t *e)
{
    s_log_shown = static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e)));
    refilter_log();
}

void log_warnings_cb(lv_event_t *)
{
    s_log_warnings = !s_log_warnings;
    refilter_log();
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
    for (int i = 0; i < s_card_count; ++i) {
        lv_obj_set_hidden(s_detail[i], i != index);
    }
    theme::set_text(s_detail_title, s_cards[index].title);
    lv_obj_scroll_to_y(s_detail[index], 0, LV_ANIM_OFF);
    s_diagnostics->resize(DETAIL_W, s_detail_height[index]);
    s_diagnostics->open();
}

void log_held_cb(lv_event_t *e)
{
    s_tile_long = true;
    open_log(static_cast<int>(reinterpret_cast<std::intptr_t>(lv_event_get_user_data(e))));
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

    write_percent(s_volume_value, percent);

    if (s_handlers.volume != nullptr) {
        s_handlers.volume(percent, lv_event_get_code(e) == LV_EVENT_RELEASED);
    }
}

namespace {
lv_obj_t *make_tile_glyph(lv_obj_t *tile, const char *icon, const lv_image_dsc_t *image)
{
    if (image == nullptr) {
        return theme::make_accent_label(tile, icon, fonts::size_28());
    }
    lv_obj_t *glyph = lv_image_create(tile);
    lv_image_set_src(glyph, image);
    lv_obj_set_style_image_recolor(glyph, lv_color_hex(theme::primary), 0);
    lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
    lv_obj_set_clickable(glyph, false);
    return glyph;
}

/** The line along a tile's foot: a note or a reading, cut short with dots. */
lv_obj_t *make_tile_foot(lv_obj_t *tile, std::int32_t w, const char *initial,
                         std::uint32_t colour, const lv_font_t *font)
{
    lv_obj_t *label = theme::make_label(tile, initial, colour, font);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_width(label, w - 2 * TILE_PAD);
    lv_obj_set_height(label, font->line_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}
}  // namespace

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
    lv_obj_set_style_pad_all(tile, TILE_PAD, 0);

    lv_obj_align(make_tile_glyph(tile, icon, image), LV_ALIGN_TOP_LEFT, 0, 0);

    const lv_font_t *font = titled ? theme::type_title() : fonts::size_22();
    lv_obj_t *caption     = theme::make_label(tile, title,
                                              titled ? theme::text : theme::secondary, font);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, CAPTION_X, titled ? 0 : CAPTION_READING_Y);
    lv_obj_set_width(caption, w - 2 * TILE_PAD - CAPTION_X - CAPTION_END_GAP);
    lv_obj_set_height(caption, font->line_height);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_DOTS);
    return tile;
}

lv_obj_t *tile_note(lv_obj_t *tile, std::int32_t w, const char *initial)
{
    return make_tile_foot(tile, w, initial, theme::secondary, theme::type_body());
}

namespace {
lv_obj_t *tile_value(lv_obj_t *tile, std::int32_t w, const char *initial)
{
    return make_tile_foot(tile, w, initial, theme::text, fonts::size_32());
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
    lv_obj_set_style_pad_column(row, INFO_ROW_GAP, 0);

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
    const Card &spec = s_cards[index];
    lv_obj_t   *tile = build_tile(parent, x, y, w, h, glyph_symbol(spec.glyph), spec.title, false,
                                  glyph_image(spec.glyph));
    lv_obj_add_event_cb(tile, detail_clicked_cb, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));
    lv_obj_add_event_cb(tile, log_held_cb, LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<std::intptr_t>(index)));

    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, TILE_DOT, TILE_DOT);
    theme::style_panel(dot, theme::secondary, TILE_DOT / 2);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, TILE_DOT_Y);
    lv_obj_set_clickable(dot, false);

    s_tile_value[index] = tile_value(tile, w, "--");
    s_tile_dot[index]   = dot;
}
namespace {
std::int32_t detail_height(const Card &card)
{
    const std::int32_t body = card.row_count * DETAIL_ROW_H + (card.row_count - 1) * DETAIL_ROW_GAP;

    const Layout       l   = layout();
    const std::int32_t cap =
        std::min(l.screen_h * DETAIL_MAX_PERCENT / WHOLE_PERCENT, l.content_h - 2 * GAP);
    return std::min(2 * DETAIL_PAD + modal_body_y() + body, cap);
}

lv_obj_t *build_detail_panel(lv_obj_t *card, int index, std::int32_t width, std::int32_t body_y)
{
    const std::int32_t height = s_detail_height[index] - 2 * DETAIL_PAD - body_y;
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

    const Card &spec = s_cards[index];
    for (int r = 0; r < spec.row_count && r < kMaxRows; ++r) {
        s_row_value[index][r] = build_info_row(panel, spec.rows[r], fonts::size_20(), DETAIL_ROW_H);
    }
    return panel;
}
}  // namespace

void build_detail_overlay(lv_obj_t *parent)
{
    s_diagnostics.emplace(parent, DETAIL_W,
                          s_card_count > 0 ? detail_height(s_cards[0]) : DETAIL_H_WITHOUT_CARDS);
    lv_obj_t *card = s_diagnostics->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    s_detail_title = theme::make_accent_label(card, "", fonts::size_28());
    lv_obj_align(s_detail_title, LV_ALIGN_TOP_LEFT, 0, MODAL_TITLE_Y);

    const std::int32_t width  = DETAIL_W - 2 * DETAIL_PAD;
    const std::int32_t body_y = modal_body_y();

    for (int i = 0; i < s_card_count; ++i) {
        s_detail_height[i] = detail_height(s_cards[i]);
        s_detail[i]        = build_detail_panel(card, i, width, body_y);
    }

    s_diagnostics->add_close_button();
}

void show_log_cb(lv_event_t *)
{
    open_log(LOG_ALL_CARDS);
}

namespace {
// Whose lines: all of them, or one card's. Scrolls sideways when they do not fit.
void build_log_chips(lv_obj_t *card, std::int32_t y, std::int32_t width)
{
    lv_obj_t *chips = lv_obj_create(card);
    lv_obj_set_pos(chips, 0, y);
    lv_obj_set_size(chips, width, CHIP_H);
    theme::style_panel(chips, theme::panel, 0);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chips, CHIP_GAP, 0);
    lv_obj_set_scrollable(chips, true);  // the panel style turns it off
    lv_obj_set_scroll_dir(chips, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(chips, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i <= s_card_count; ++i) {
        const char *name = i == 0 ? "All" : s_cards[chip_card(i)].title;
        lv_obj_t   *chip = theme::make_button(chips, name, theme::panel_light, fonts::size_20());
        lv_obj_set_size(chip, LV_SIZE_CONTENT, CHIP_H);
        lv_obj_set_style_pad_hor(chip, CHIP_PAD_X, 0);
        theme::fill_accent(chip, LV_STATE_CHECKED);
        lv_obj_add_event_cb(chip, log_chip_cb, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<std::intptr_t>(chip_card(i))));
        s_log_chip[i] = chip;
    }
}

void build_log_warnings_button(lv_obj_t *card)
{
    s_log_warn = theme::make_button(card, "Warnings only", theme::panel_light, fonts::size_20());
    lv_obj_set_size(s_log_warn, WARN_W, ModalOverlay::header_height());
    lv_obj_align(s_log_warn, LV_ALIGN_TOP_RIGHT, -(ModalOverlay::header_height() + CHIP_GAP),
                 WARN_Y);
    theme::fill_accent(s_log_warn, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_log_warn, log_warnings_cb, LV_EVENT_CLICKED, nullptr);
}

void build_log_pane(lv_obj_t *card, std::int32_t y, std::int32_t width, std::int32_t height)
{
    lv_obj_t *pane = lv_obj_create(card);
    lv_obj_set_pos(pane, 0, y);
    lv_obj_set_size(pane, width, height);
    theme::style_panel(pane, theme::background, LOG_PANE_RADIUS);
    lv_obj_set_style_pad_all(pane, LOG_PANE_PAD, 0);
    lv_obj_set_scrollable(pane, true);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);

    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, LOG_LINE_GAP, 0);
    s_log_pane = pane;

    for (lv_obj_t *&line : s_log_line) {
        line = theme::make_label(pane, "", theme::secondary, fonts::size_16());
        lv_obj_set_width(line, width - 2 * LOG_PANE_PAD);
        lv_label_set_long_mode(line, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_hidden(line, true);
    }
    s_log_empty = theme::make_label(pane, "Nothing logged yet", theme::secondary,
                                    fonts::size_16());
}
}  // namespace

void build_log_overlay(lv_obj_t *parent)
{
    s_log_modal.emplace(parent, LOG_W, LOG_H);
    lv_obj_t *card = s_log_modal->content();
    lv_obj_set_style_pad_all(card, DETAIL_PAD, 0);

    lv_obj_t *title = theme::make_accent_label(card, "Log", fonts::size_28());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, MODAL_TITLE_Y);

    const std::int32_t width  = LOG_W - 2 * DETAIL_PAD;
    const std::int32_t chip_y = modal_body_y();
    const std::int32_t body_y = chip_y + CHIP_H + LOG_GAP;
    const std::int32_t height = LOG_H - 2 * DETAIL_PAD - body_y;

    build_log_chips(card, chip_y, width);
    build_log_warnings_button(card);
    build_log_pane(card, body_y, width, height);

    s_log_modal->add_close_button();
    lv_timer_create(log_tick, LOG_REFRESH_MS, nullptr);
}

lv_obj_t *build_slider_card(lv_obj_t *parent, std::int32_t y, std::int32_t w, const char *icon,
                            const char *title, int value, int low, lv_event_cb_t changed,
                            lv_obj_t **out_value)
{
    lv_obj_t *root = build_row_card(parent, y, w, icon, nullptr);

    lv_obj_t *caption = theme::make_label(root, title, theme::secondary, fonts::size_22());
    lv_obj_set_width(caption, SLIDER_CAPTION_W);

    lv_obj_t *slider = lv_slider_create(root);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, SLIDER_H);
    lv_obj_set_ext_click_area(slider, SLIDER_TOUCH_MARGIN);
    lv_slider_set_range(slider, low, WHOLE_PERCENT);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(theme::panel), LV_PART_MAIN);
    theme::fill_accent(slider, LV_PART_INDICATOR);
    theme::fill_accent(slider, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, SLIDER_KNOB_PAD, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, changed, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, changed, LV_EVENT_RELEASED, nullptr);

    *out_value = theme::make_label(root, "", theme::text, fonts::size_22());
    write_percent(*out_value, value);
    lv_obj_set_width(*out_value, SLIDER_VALUE_W);
    lv_obj_set_style_text_align(*out_value, LV_TEXT_ALIGN_RIGHT, 0);
    return slider;
}

}  // namespace ui::detail
