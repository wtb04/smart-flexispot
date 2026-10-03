#include "popout.h"

#include "ui_internal.h"

#include <algorithm>

namespace ui::detail {
namespace {
constexpr int           WHOLE    = 1000;  // all of it shown, in thousandths
constexpr std::uint32_t OPEN_MS  = 240;
constexpr std::uint32_t CLOSE_MS = 180;
constexpr std::uint32_t IDLE_CHECK_MS = 1000;

Popout *s_out = nullptr;  // the one out, or on its way

// What a frame shows from the corner, `w` across and `h` down.
lv_area_t shown_area(const Popout &p, std::int32_t w, std::int32_t h)
{
    const std::int32_t y1 = p.upward ? p.y - h : p.y;
    return p.leftward ? lv_area_t{p.x - w, y1, p.x - 1, y1 + h - 1} : lv_area_t{p.x, y1, p.x + w - 1, y1 + h - 1};
}

void reveal(Popout &p, int shown)
{
    const std::int32_t w    = p.full_w * shown / WHOLE;
    const std::int32_t h    = p.full_h * shown / WHOLE;
    lv_display_t      *disp = lv_display_get_default();
    lv_display_enable_invalidation(disp, false);
    lv_obj_set_hidden(p.frame, shown <= 0);
    lv_obj_set_pos(p.frame, p.leftward ? p.x - w : p.x, p.upward ? p.y - h : p.y);
    lv_obj_set_size(p.frame, std::max<std::int32_t>(w, 1), std::max<std::int32_t>(h, 1));
    const std::int32_t card_x = p.leftward ? w - p.near - lv_obj_get_width(p.card) : p.near;
    const std::int32_t card_y = p.upward ? h - p.top - lv_obj_get_height(p.card) : p.top;
    lv_obj_set_pos(p.card, card_x, card_y);
    lv_obj_set_pos(p.ring, card_x - GAP, card_y - GAP);
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
        const lv_area_t down = p.upward ? lv_area_t{small.x1, big.y1, small.x2, small.y1 - 1}
                                        : lv_area_t{small.x1, small.y2 + 1, small.x2, big.y2};
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

    // It lies over the page, so a ring of background sets it apart: a shape of
    // its own under the card, as an outline left a hairline of the page showing
    // where its edge and the card's were both smoothed at the corners.
    p.ring = lv_obj_create(p.frame);
    lv_obj_remove_style_all(p.ring);
    theme::style_panel(p.ring, theme::background, theme::radius::card + GAP);
    lv_obj_set_clickable(p.ring, false);
    lv_obj_set_scrollable(p.ring, false);

    p.card = lv_obj_create(p.frame);
    lv_obj_set_width(p.card, w);
    theme::style_panel(p.card, theme::panel, theme::radius::card);
    lv_obj_set_scrollable(p.card, false);

    // Out while anything is touched, its buttons too, which a press on the card
    // itself would not catch, as LVGL keeps a child's presses to the child.
    p.idle_ms = idle_ms;
    p.idle    = lv_timer_create([](lv_timer_t *t) {
        Popout &out = *static_cast<Popout *>(lv_timer_get_user_data(t));
        if (lv_display_get_inactive_time(nullptr) >= out.idle_ms) {
            open_popout(out, false);
        }
    }, IDLE_CHECK_MS, &p);
    lv_timer_pause(p.idle);
    return p.card;
}

void place_popout(Popout &p, std::int32_t x, std::int32_t y, bool leftward, bool upward)
{
    if (p.x == x && p.y == y && p.leftward == leftward && p.upward == upward) {
        return;
    }
    if (p.shown > 0) {
        lv_obj_invalidate(lv_screen_active());  // where it was, and where it goes
    }
    p.x        = x;
    p.y        = y;
    p.leftward = leftward;
    p.upward   = upward;
    p.shown_w = p.shown_h = 0;
    reveal(p, p.shown);
}

void fit_popout(Popout &p)
{
    lv_obj_update_layout(p.card);
    lv_obj_set_size(p.ring, lv_obj_get_width(p.card) + 2 * GAP, lv_obj_get_height(p.card) + 2 * GAP);
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
    if (p.lit != nullptr) {
        p.lit(open);
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

void close_popout()
{
    if (s_out != nullptr) {
        open_popout(*s_out, false);
    }
}

bool popout_moving(const Popout &p)
{
    return lv_anim_get(const_cast<Popout *>(&p), nullptr) != nullptr;
}

}  // namespace ui::detail
