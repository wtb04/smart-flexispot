#include "ui_internal.h"

#include "notices_model.h"
#include "status_model.h"
#include "topics.h"

#include <cctype>
#include <ctime>

namespace ui::detail {
namespace {
constexpr int          NOTIFY_DEFAULT_MS = 8000;
constexpr std::int32_t NOTIFY_W          = 680;
constexpr std::int32_t NOTIFY_PAD        = theme::space::xl;
constexpr std::int32_t INNER_W           = NOTIFY_W - 2 * NOTIFY_PAD;
constexpr lv_opa_t     SCRIM_OPA         = LV_OPA_70;

constexpr std::int32_t DOT          = 10;
constexpr std::int32_t HEADLINE_GAP  = theme::space::m;   // under the eyebrow
constexpr std::int32_t MESSAGE_GAP   = theme::space::s;   // under the headline
constexpr std::int32_t LINE_GAP      = theme::space::l;   // over the line that runs down
constexpr std::int32_t LINE_H        = 4;
constexpr int          HEADLINE_ROWS = 2;
constexpr int          MESSAGE_ROWS  = 3;
constexpr int          LINE_STEPS   = 1000;
constexpr time_t       CLOCK_SET    = 1'700'000'000;  // any earlier and the clock is not set yet
}  // namespace

lv_obj_t   *s_notice_scrim = nullptr;
lv_obj_t   *s_notice_card  = nullptr;
namespace {
lv_obj_t   *s_notice_dot   = nullptr;
lv_obj_t   *s_notice_source = nullptr;  // the eyebrow: where it came from
lv_obj_t   *s_notice_time   = nullptr;  // when, and how many wait behind it
lv_obj_t   *s_notice_title  = nullptr;  // the headline
lv_obj_t   *s_notice_body   = nullptr;
lv_obj_t   *s_notice_line   = nullptr;  // runs down until the notice goes
lv_timer_t *s_notice_timer = nullptr;
char        s_notice_clock[8] = "";  // when the notice on show came

std::uint32_t notice_ink(Level level)
{
    switch (level) {
        case Level::Bad:  return theme::red;
        case Level::Warn: return theme::amber;
        case Level::Good: return theme::green;
        default:          return theme::primary;
    }
}

void hide_notice()
{
    lv_obj_set_hidden(s_notice_card, true);
    lv_obj_set_hidden(s_notice_scrim, true);
    lv_anim_delete(s_notice_line, nullptr);
    if (s_notice_timer != nullptr) {
        lv_timer_pause(s_notice_timer);
    }
}

}  // namespace

// When it came, and how many wait behind it.
void paint_notice_corner()
{
    char corner[32];
    if (notices_state().count > 0) {
        std::snprintf(corner, sizeof(corner), "%d MORE, %s", notices_state().count, s_notice_clock);
    } else {
        std::snprintf(corner, sizeof(corner), "%s", s_notice_clock);
    }
    theme::set_text(s_notice_time, corner);
}

namespace {
void upper_into(char *out, std::size_t size, const char *text)
{
    std::size_t i = 0;
    for (; text[i] != '\0' && i + 1 < size; ++i) {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[i])));
    }
    out[i] = '\0';
}

void write_notice(const Notice &notice)
{
    const std::uint32_t ink = notice_ink(notice.level);
    lv_obj_set_style_bg_color(s_notice_dot, lv_color_hex(ink), 0);
    lv_obj_set_style_bg_color(s_notice_line, lv_color_hex(ink), LV_PART_INDICATOR);

    char eyebrow[sizeof(notice.source)];
    upper_into(eyebrow, sizeof(eyebrow), notice.source[0] != '\0' ? notice.source : "Notice");
    theme::set_text(s_notice_source, eyebrow);

    // The title is the headline; without one, the message is.
    const bool titled = notice.title[0] != '\0';
    theme::set_text(s_notice_title, titled ? notice.title : notice.message);
    theme::set_text(s_notice_body, titled ? notice.message : "");
    lv_obj_set_hidden(s_notice_body, !titled || notice.message[0] == '\0');

    const time_t now = std::time(nullptr);
    s_notice_clock[0] = '\0';
    if (now >= CLOCK_SET) {
        std::tm local{};
        localtime_r(&now, &local);
        std::strftime(s_notice_clock, sizeof(s_notice_clock), "%H:%M", &local);
    }
    paint_notice_corner();
}

}  // namespace

// Over the pages and the top row, the dock left clear, or over the whole screen
// when a view has it all and the dock is out of sight.
void place_notice()
{
    const Layout l    = layout();
    const bool   full = fullscreen_open();
    const std::int32_t dock = DOCK_W + GAP;
    lv_obj_set_pos(s_notice_scrim, full || l.dock_right ? 0 : dock, 0);
    lv_obj_set_size(s_notice_scrim, full ? l.screen_w : l.screen_w - dock, l.screen_h);
    if (full) {
        lv_obj_align(s_notice_card, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_align(s_notice_card, LV_ALIGN_CENTER, l.content_x + l.content_w / 2 - l.screen_w / 2,
                     l.content_y + l.content_h / 2 - l.screen_h / 2);
    }
}

namespace {
void raise_notice()
{
    place_notice();
    lv_obj_set_hidden(s_notice_scrim, false);
    lv_obj_move_foreground(s_notice_scrim);
    lv_obj_set_hidden(s_notice_card, false);
    lv_obj_move_foreground(s_notice_card);
}

void line_step(void *line, std::int32_t value)
{
    lv_bar_set_value(static_cast<lv_obj_t *>(line), value, LV_ANIM_OFF);
}

void restart_notice_timer(int timeout_ms)
{
    lv_anim_delete(s_notice_line, nullptr);
    // Kept until tapped, it has no time to run down.
    lv_obj_set_hidden(s_notice_line, timeout_ms < 0);
    if (timeout_ms < 0) {
        if (s_notice_timer != nullptr) {
            lv_timer_pause(s_notice_timer);
        }
        return;
    }
    const int timeout = timeout_ms > 0 ? timeout_ms : NOTIFY_DEFAULT_MS;
    if (s_notice_timer != nullptr) {
        lv_timer_set_period(s_notice_timer, static_cast<std::uint32_t>(timeout));
        lv_timer_reset(s_notice_timer);
        lv_timer_resume(s_notice_timer);
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_notice_line);
    lv_anim_set_exec_cb(&anim, line_step);
    lv_anim_set_values(&anim, LINE_STEPS, 0);
    lv_anim_set_duration(&anim, static_cast<std::uint32_t>(timeout));
    lv_anim_start(&anim);
}
}  // namespace

void show_next_notice()
{
    Notice notice{};
    if (!notices_take_next(notice)) {
        hide_notice();
        if (s_notice_lit_screen) {
            s_notice_lit_screen = false;
            set_screen_state(false);
        }
        return;
    }
    if (!status_state().screen_on) {
        s_notice_lit_screen = true;
        set_screen_state(true);
    }
    write_notice(notice);
    raise_notice();
    restart_notice_timer(notice.timeout_ms);
}
namespace {
void notice_timeout_cb(lv_timer_t *) { show_next_notice(); }
void notice_tapped_cb(lv_event_t *) { show_next_notice(); }

lv_obj_t *make_notice_scrim()
{
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    theme::style_panel(scrim, theme::background, 0);
    lv_obj_set_style_bg_opa(scrim, SCRIM_OPA, 0);
    lv_obj_set_hidden(scrim, true);
    lv_obj_set_clickable(scrim, true);
    lv_obj_add_event_cb(scrim, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);
    return scrim;
}

// A card as the pages have them, on the scrim: a dot in the notice's colour
// and what it is about over the message, the time it came at the right, and
// along the foot a line running down until it goes. A tap moves it on.
lv_obj_t *make_notice_frame()
{
    lv_obj_t *card = lv_obj_create(lv_layer_top());
    lv_obj_set_width(card, NOTIFY_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    theme::style_panel(card, theme::panel_light, theme::radius::card);
    lv_obj_set_style_pad_all(card, NOTIFY_PAD, 0);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_hidden(card, true);
    lv_obj_add_event_cb(card, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);
    return card;
}

lv_obj_t *quiet_box(lv_obj_t *parent, std::int32_t w, std::int32_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_clickable(box, false);
    lv_obj_set_scrollable(box, false);
    return box;
}

void build_eyebrow(lv_obj_t *card)
{
    const std::int32_t row_h = theme::type_label()->line_height;
    lv_obj_t          *row   = quiet_box(card, INNER_W, row_h);
    s_notice_dot             = lv_obj_create(row);
    theme::style_panel(s_notice_dot, theme::primary, theme::radius::pill);
    lv_obj_set_size(s_notice_dot, DOT, DOT);
    lv_obj_align(s_notice_dot, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_clickable(s_notice_dot, false);
    s_notice_source = theme::make_eyebrow(row, "");
    lv_obj_align(s_notice_source, LV_ALIGN_LEFT_MID, DOT + theme::space::s, 0);
    s_notice_time = theme::make_eyebrow(row, "");
    lv_obj_align(s_notice_time, LV_ALIGN_RIGHT_MID, 0, 0);
}

lv_obj_t *wrapped(lv_obj_t *card, std::uint32_t colour, const lv_font_t *font, int rows,
                  std::int32_t gap)
{
    lv_obj_t *label = theme::make_label(card, "", colour, font);
    lv_obj_set_width(label, INNER_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_height(label, rows * lv_font_get_line_height(font), 0);
    lv_obj_set_style_margin_top(label, gap, 0);
    return label;
}

void build_foot(lv_obj_t *card)
{
    s_notice_line = lv_bar_create(card);
    lv_obj_set_size(s_notice_line, INNER_W, LINE_H);
    lv_obj_set_style_margin_top(s_notice_line, LINE_GAP, 0);
    lv_bar_set_range(s_notice_line, 0, LINE_STEPS);
    theme::style_panel(s_notice_line, theme::panel, LINE_H / 2);
    lv_obj_set_style_radius(s_notice_line, LINE_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_notice_line, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_clickable(s_notice_line, false);
}
}  // namespace

void create_notice_card()
{
    s_notice_scrim = make_notice_scrim();
    s_notice_card  = make_notice_frame();
    build_eyebrow(s_notice_card);
    s_notice_title = wrapped(s_notice_card, theme::text, theme::type_title(), HEADLINE_ROWS, HEADLINE_GAP);
    s_notice_body  = wrapped(s_notice_card, theme::secondary, theme::type_value(), MESSAGE_ROWS, MESSAGE_GAP);
    build_foot(s_notice_card);

    s_notice_timer = lv_timer_create(notice_timeout_cb, NOTIFY_DEFAULT_MS, nullptr);
    lv_timer_pause(s_notice_timer);

    // The next as it comes, once the splash is out of the way; while one shows,
    // how many wait behind it.
    const auto follow = [] {
        if (!status_state().splash_gone) {
            return;
        }
        if (!lv_obj_is_hidden(s_notice_card)) {
            paint_notice_corner();
        } else if (notices_state().count > 0) {
            show_next_notice();
        }
    };
    subscribe(Topic::Notices, kNoView, follow);
    subscribe(Topic::Status, kNoView, follow);
}

}  // namespace ui::detail
