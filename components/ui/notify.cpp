#include "ui_internal.h"

namespace ui::detail {
namespace {
constexpr int          NOTIFY_DEFAULT_MS = 8000;
constexpr std::int32_t NOTIFY_W          = 640;
constexpr std::int32_t NOTIFY_H          = 220;
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
    const Notice notice = s_notice_queue[0];
    for (int i = 1; i < s_notice_count; ++i) {
        s_notice_queue[i - 1] = s_notice_queue[i];
    }
    --s_notice_count;

    const NoticeInk ink = notice_ink(notice.level);
    theme::fill_accent_or(s_notice_bar, ink.accent, ink.colour);
    if (notice.title[0] != '\0') {
        lv_label_set_text(s_notice_title, notice.title);
        lv_label_set_text(s_notice_body, notice.message);
    } else {
        lv_label_set_text(s_notice_title, notice.message);
        lv_label_set_text(s_notice_body, "");
    }

    lv_obj_set_hidden(s_notice_scrim, false);
    lv_obj_move_foreground(s_notice_scrim);
    lv_obj_set_hidden(s_notice_card, false);
    lv_obj_move_foreground(s_notice_card);

    const int timeout = notice.timeout_ms > 0 ? notice.timeout_ms : NOTIFY_DEFAULT_MS;
    if (s_notice_timer != nullptr) {
        lv_timer_set_period(s_notice_timer, static_cast<std::uint32_t>(timeout));
        lv_timer_reset(s_notice_timer);
        lv_timer_resume(s_notice_timer);
    }
}
namespace {
void notice_timeout_cb(lv_timer_t *) { show_next_notice(); }
void notice_tapped_cb(lv_event_t *) { show_next_notice(); }
}  // namespace

void create_notice_card()
{
    const Layout l = layout();

    s_notice_scrim = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(s_notice_scrim, l.rail_right ? 0 : RAIL_W, 0);
    lv_obj_set_size(s_notice_scrim, l.screen_w - RAIL_W, l.screen_h);
    theme::style_panel(s_notice_scrim, theme::background, 0);
    lv_obj_set_style_bg_opa(s_notice_scrim, LV_OPA_70, 0);
    lv_obj_set_hidden(s_notice_scrim, true);
    lv_obj_set_clickable(s_notice_scrim, true);
    lv_obj_add_event_cb(s_notice_scrim, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);

    s_notice_card = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_notice_card, NOTIFY_W, NOTIFY_H);
    lv_obj_align(s_notice_card, LV_ALIGN_CENTER,
                 l.content_x + l.content_w / 2 - l.screen_w / 2,
                 GAP + l.content_h / 2 - l.screen_h / 2);
    theme::style_panel(s_notice_card, theme::panel_light);
    lv_obj_set_style_border_width(s_notice_card, 2, 0);
    lv_obj_set_style_border_color(s_notice_card, lv_color_hex(theme::panel_light), 0);
    lv_obj_set_hidden(s_notice_card, true);
    lv_obj_add_event_cb(s_notice_card, notice_tapped_cb, LV_EVENT_CLICKED, nullptr);

    s_notice_bar = lv_obj_create(s_notice_card);
    lv_obj_set_size(s_notice_bar, 10, NOTIFY_H - 40);
    lv_obj_align(s_notice_bar, LV_ALIGN_LEFT_MID, -8, 0);
    lv_obj_set_style_border_width(s_notice_bar, 0, 0);
    lv_obj_set_style_radius(s_notice_bar, 5, 0);

    s_notice_title = lv_label_create(s_notice_card);
    lv_obj_set_width(s_notice_title, NOTIFY_W - 80);
    lv_label_set_long_mode(s_notice_title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_notice_title, LV_ALIGN_TOP_LEFT, 24, 8);
    lv_obj_set_style_text_font(s_notice_title, fonts::size_28(), 0);
    lv_obj_set_style_text_color(s_notice_title, lv_color_hex(theme::text), 0);

    s_notice_body = lv_label_create(s_notice_card);
    lv_obj_set_width(s_notice_body, NOTIFY_W - 80);
    lv_label_set_long_mode(s_notice_body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_notice_body, LV_ALIGN_TOP_LEFT, 24, 76);
    lv_obj_set_style_text_color(s_notice_body, lv_color_hex(theme::secondary), 0);
    lv_obj_set_style_text_font(s_notice_body, fonts::size_28(), 0);

    s_notice_timer = lv_timer_create(notice_timeout_cb, NOTIFY_DEFAULT_MS, nullptr);
    lv_timer_pause(s_notice_timer);
}

}  // namespace ui::detail
