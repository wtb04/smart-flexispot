#include "popout.h"

#include "ui_internal.h"

#include <algorithm>

namespace ui::detail {
namespace {
constexpr int           WHOLE    = 1000;  // all of it shown, in thousandths
constexpr std::uint32_t OPEN_MS  = 240;
constexpr std::uint32_t CLOSE_MS = 180;

Popout *s_out = nullptr;  // the one out, or on its way

// What a frame shows from the corner, `w` across and `h` down.
lv_area_t shown_area(const Popout &p, std::int32_t w, std::int32_t h)
{
    return p.leftward ? lv_area_t{p.x - w, p.y, p.x - 1, p.y + h - 1} : lv_area_t{p.x, p.y, p.x + w - 1, p.y + h - 1};
}

void reveal(Popout &p, int shown)
{
    const std::int32_t w    = p.full_w * shown / WHOLE;
    const std::int32_t h    = p.full_h * shown / WHOLE;
    lv_display_t      *disp = lv_display_get_default();
    lv_display_enable_invalidation(disp, false);
    lv_obj_set_hidden(p.frame, shown <= 0);
    lv_obj_set_pos(p.frame, p.leftward ? p.x - w : p.x, p.y);
    lv_obj_set_size(p.frame, std::max<std::int32_t>(w, 1), std::max<std::int32_t>(h, 1));
    lv_obj_set_pos(p.card, p.leftward ? w - p.near - lv_obj_get_width(p.card) : p.near, p.top);
    lv_obj_update_layout(p.frame);  // moved now, while that draws nothing again
    lv_display_enable_invalidation(disp, true);

    // One of the two holds the other: what lies in the larger alone changed.
    const std::int32_t big_w = std::max(w, p.shown_w), big_h = std::max(h, p.shown_h);
    const std::int32_t small_w = std::min(w, p.shown_w), small_h = std::min(h, p.shown_h);
    p.shown   = shown;
    p.shown_w = w;
    p.shown_h = h;
    const lv_area_t big   = shown_area(p, big_w, big_h);
    const lv_area_t small = shown_area(p, small_w, small_h);
    lv_obj_t       *scr   = lv_screen_active();
    if (big_w > small_w && big_h > 0) {
        const lv_area_t across = p.leftward ? lv_area_t{big.x1, big.y1, small.x1 - 1, big.y2}
                                            : lv_area_t{small.x2 + 1, big.y1, big.x2, big.y2};
        lv_obj_invalidate_area(scr, &across);
    }
    if (big_h > small_h && small_w > 0) {
        const lv_area_t down{small.x1, p.y + small_h, small.x2, big.y2};
        lv_obj_invalidate_area(scr, &down);
    }
}

void shown_cb(void *var, std::int32_t value)
{
    reveal(*static_cast<Popout *>(var), value);
}
}  // namespace

lv_obj_t *build_popout(Popout &p, lv_obj_t *parent, std::int32_t w, std::int32_t near, std::int32_t top,
                       std::uint32_t idle_ms)
{
    const Layout l = layout();
    p.near         = near;
    p.top          = top;
    p.scrim        = lv_obj_create(parent);
    lv_obj_remove_style_all(p.scrim);
    lv_obj_set_size(p.scrim, l.screen_w, l.screen_h);
    lv_obj_add_event_cb(p.scrim, [](lv_event_t *e) { open_popout(*static_cast<Popout *>(lv_event_get_user_data(e)), false); },
                        LV_EVENT_CLICKED, &p);
    lv_obj_set_hidden(p.scrim, true);

    p.frame = lv_obj_create(parent);
    lv_obj_remove_style_all(p.frame);
    lv_obj_set_clickable(p.frame, false);  // taps on the ring go to the scrim
    lv_obj_set_scrollable(p.frame, false);
    lv_obj_set_hidden(p.frame, true);

    p.card = lv_obj_create(p.frame);
    lv_obj_set_width(p.card, w);
    theme::style_panel(p.card, theme::panel, theme::radius::card);
    // It lies over the page, so a ring of background sets it apart.
    lv_obj_set_style_outline_width(p.card, GAP, 0);
    lv_obj_set_style_outline_color(p.card, lv_color_hex(theme::background), 0);
    lv_obj_set_style_outline_opa(p.card, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(p.card, false);
    // A press on it keeps it out as long as it is being used.
    lv_obj_add_event_cb(p.card, [](lv_event_t *e) { lv_timer_reset(static_cast<Popout *>(lv_event_get_user_data(e))->idle); },
                        LV_EVENT_PRESSED, &p);

    p.idle = lv_timer_create([](lv_timer_t *t) { open_popout(*static_cast<Popout *>(lv_timer_get_user_data(t)), false); },
                             idle_ms, &p);
    lv_timer_pause(p.idle);
    return p.card;
}

void place_popout(Popout &p, std::int32_t x, std::int32_t y, bool leftward)
{
    if (p.x == x && p.y == y && p.leftward == leftward) {
        return;
    }
    if (p.shown > 0) {
        lv_obj_invalidate(lv_screen_active());  // where it was, and where it goes
    }
    p.x        = x;
    p.y        = y;
    p.leftward = leftward;
    p.shown_w = p.shown_h = 0;
    reveal(p, p.shown);
}

void fit_popout(Popout &p)
{
    lv_obj_update_layout(p.card);
    p.full_w = p.near + lv_obj_get_width(p.card) + GAP;
    p.full_h = p.top + lv_obj_get_height(p.card) + GAP;
    reveal(p, p.shown);
}

void open_popout(Popout &p, bool open)
{
    if (p.card == nullptr || open == p.open) {
        return;
    }
    if (open && s_out != nullptr && s_out != &p) {
        open_popout(*s_out, false);
    }
    p.open = open;
    if (open) {
        s_out = &p;
    } else if (s_out == &p) {
        s_out = nullptr;
    }
    if (p.button != nullptr) {
        lv_obj_set_state(p.button, LV_STATE_CHECKED, open);
    }
    // The scrim is clear and what stays over it lies beside the card, so none
    // of them changes a pixel by coming or going; drawn again, they would cost
    // the whole screen.
    lv_display_t *disp = lv_display_get_default();
    lv_display_enable_invalidation(disp, false);
    lv_obj_set_hidden(p.scrim, !open);
    if (open) {
        lv_obj_move_foreground(p.scrim);
        lv_obj_move_foreground(p.frame);
        if (p.above != nullptr) {
            lv_obj_move_foreground(p.above);
        }
    }
    lv_display_enable_invalidation(disp, true);
    if (open) {
        lv_timer_reset(p.idle);
        lv_timer_resume(p.idle);
    } else {
        lv_timer_pause(p.idle);
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &p);
    lv_anim_set_exec_cb(&anim, shown_cb);
    lv_anim_set_values(&anim, p.shown, open ? WHOLE : 0);
    lv_anim_set_duration(&anim, open ? OPEN_MS : CLOSE_MS);
    lv_anim_set_path_cb(&anim, open ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&anim);
}

bool popout_moving(const Popout &p)
{
    return lv_anim_get(const_cast<Popout *>(&p), nullptr) != nullptr;
}

}  // namespace ui::detail
