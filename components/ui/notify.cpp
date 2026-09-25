#include "ui_internal.h"

namespace ui::detail {
namespace {
constexpr int          NOTIFY_DEFAULT_MS = 8000;
constexpr std::int32_t NOTIFY_W          = 640;
constexpr std::int32_t NOTIFY_H          = 220;
constexpr std::int32_t NOTIFY_BORDER_W   = 2;
constexpr lv_opa_t     SCRIM_OPA         = LV_OPA_70;

constexpr std::int32_t BAR_W        = 10;
constexpr std::int32_t BAR_MARGIN_V = 20;
constexpr std::int32_t BAR_H        = NOTIFY_H - 2 * BAR_MARGIN_V;
constexpr std::int32_t BAR_X        = -8;
constexpr std::int32_t BAR_RADIUS   = BAR_W / 2;

constexpr std::int32_t TEXT_X          = theme::space::l;
constexpr std::int32_t TEXT_ROOM_RIGHT = 56;
constexpr std::int32_t TEXT_W          = NOTIFY_W - TEXT_X - TEXT_ROOM_RIGHT;
constexpr std::int32_t TITLE_Y         = 8;
constexpr std::int32_t BODY_Y          = 76;
}  // namespace

Notice      s_notice_queue[NOTIFY_QUEUE_LEN];
int         s_notice_count = 0;
lv_obj_t   *s_notice_scrim = nullptr;
lv_obj_t   *s_notice_card  = nullptr;
namespace {
lv_obj_t   *s_notice_bar   = nullptr;
lv_obj_t   *s_notice_title = nullptr;
lv_obj_t   *s_notice_body  = nullptr;
lv_timer_t *s_notice_timer = nullptr;

struct NoticeInk {
    std::uint32_t colour;
    bool          accent;
};

NoticeInk notice_ink(Level level)
{
    switch (level) {
        case Level::Bad:  return {theme::red, false};
        case Level::Warn: return {theme::amber, false};
        case Level::Good: return {theme::green, false};
        default:          return {theme::primary, true};
    }
}

void hide_notice()
{
    lv_obj_set_hidden(s_notice_card, true);
    lv_obj_set_hidden(s_notice_scrim, true);
    if (s_notice_timer != nullptr) {
        lv_timer_pause(s_notice_timer);
    }
}

Notice take_first_notice()
{
    const Notice notice = s_notice_queue[0];
    for (int i = 1; i < s_notice_count; ++i) {
        s_notice_queue[i - 1] = s_notice_queue[i];
    }
    --s_notice_count;
    return notice;
}

void write_notice(const Notice &notice)
{
    const NoticeInk ink = notice_ink(notice.level);
    theme::fill_accent_or(s_notice_bar, ink.accent, ink.colour);
    if (notice.title[0] != '\0') {
        lv_label_set_text(s_notice_title, notice.title);
        lv_label_set_text(s_notice_body, notice.message);
    } else {
        lv_label_set_text(s_notice_title, notice.message);
        lv_label_set_text(s_notice_body, "");
    }
}

void raise_notice()
{
    lv_obj_set_hidden(s_notice_scrim, false);
    lv_obj_move_foreground(s_notice_scrim);
    lv_obj_set_hidden(s_notice_card, false);
    lv_obj_move_foreground(s_notice_card);
}

void restart_notice_timer(int timeout_ms)
{
    const int timeout = timeout_ms > 0 ? timeout_ms : NOTIFY_DEFAULT_MS;
    if (s_notice_timer != nullptr) {
        lv_timer_set_period(s_notice_timer, static_cast<std::uint32_t>(timeout));
        lv_timer_reset(s_notice_timer);
        lv_timer_resume(s_notice_timer);
    }
}
}  // namespace

void show_next_notice()
{
    if (s_notice_count == 0) {
        hide_notice();
        if (s_notice_lit_screen) {
            s_notice_lit_screen = false;
            set_screen_state(false);
        }
        return;
    }
    if (!s_screen_on) {
        s_notice_lit_screen = true;
        set_screen_state(true);
    }
    const Notice notice = take_first_notice();
    write_notice(notice);
    raise_notice();
    restart_notice_timer(notice.timeout_ms);
}
namespace {
void notice_timeout_cb(lv_timer_t *) { show_next_notice(); }
void notice_tapped_cb(lv_event_t *) { show_next_notice(); }

lv_obj_t *make_notice_scrim(const Layout &l)
{
    lv_obj_t *scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(scrim, l.rail_right ? 0 : RAIL_W, 0);
    lv_obj_set_size(scrim, l.screen_w - RAIL_W, l.screen_h);
    theme::style_panel(scrim, theme::background, 0);
    lv_obj_set_style_bg_opa(scrim, SCRIM_OPA, 0);
    lv_obj_set_hidden(scrim, true);
    lv_obj_set_clickable(scrim, true);
    lv_obj_add_event_cb(scrim, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);
    return scrim;
}

lv_obj_t *make_notice_frame(const Layout &l)
{
    lv_obj_t *card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(card, NOTIFY_W, NOTIFY_H);
    lv_obj_align(card, LV_ALIGN_CENTER, l.content_x + l.content_w / 2 - l.screen_w / 2,
                 GAP + l.content_h / 2 - l.screen_h / 2);
    theme::style_panel(card, theme::panel_light);
    lv_obj_set_style_border_width(card, NOTIFY_BORDER_W, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::panel_light), 0);
    lv_obj_set_hidden(card, true);
    lv_obj_add_event_cb(card, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);
    return card;
}

lv_obj_t *make_level_bar(lv_obj_t *card)
{
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_set_size(bar, BAR_W, BAR_H);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, BAR_X, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, BAR_RADIUS, 0);
    return bar;
}

lv_obj_t *make_notice_text(lv_obj_t *card, std::int32_t y, std::uint32_t colour)
{
    lv_obj_t *label = lv_label_create(card);
    lv_obj_set_width(label, TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, TEXT_X, y);
    lv_obj_set_style_text_font(label, fonts::size_28(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    return label;
}
}  // namespace

void create_notice_card()
{
    const Layout l = layout();

    s_notice_scrim = make_notice_scrim(l);
    s_notice_card  = make_notice_frame(l);
    s_notice_bar   = make_level_bar(s_notice_card);
    s_notice_title = make_notice_text(s_notice_card, TITLE_Y, theme::text);
    s_notice_body  = make_notice_text(s_notice_card, BODY_Y, theme::secondary);

    s_notice_timer = lv_timer_create(notice_timeout_cb, NOTIFY_DEFAULT_MS, nullptr);
    lv_timer_pause(s_notice_timer);
}

}  // namespace ui::detail
