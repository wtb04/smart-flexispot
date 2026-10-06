#include "kit.h"

#include "kit_fonts.h"

#include <algorithm>

namespace ui::kit {
namespace {

constexpr std::int32_t MARGIN = space::l, GAP = space::m, PROGRESS = 1024;

struct Pop {
    lv_obj_t *scrim  = nullptr;
    lv_obj_t *card   = nullptr;
    lv_obj_t *body   = nullptr;
    lv_obj_t *source = nullptr;
    lv_area_t from{}, to{};
    bool      closing = false;
} s_pop;

lv_obj_t *s_notice = nullptr;

std::int32_t lerp(std::int32_t a, std::int32_t b, std::int32_t p) { return a + (b - a) * p / PROGRESS; }

// The card's outline grows from the source to its place while what is on it
// stays still on the screen, so it reads as opening out of what was tapped.
void grow(void *, int32_t p)
{
    if (s_pop.card == nullptr) {
        return;
    }
    const std::int32_t x = lerp(s_pop.from.x1, s_pop.to.x1, p), y = lerp(s_pop.from.y1, s_pop.to.y1, p);
    const std::int32_t w = std::max<std::int32_t>(lerp(lv_area_get_width(&s_pop.from), lv_area_get_width(&s_pop.to), p), 8);
    const std::int32_t h = std::max<std::int32_t>(lerp(lv_area_get_height(&s_pop.from), lv_area_get_height(&s_pop.to), p), 8);
    lv_obj_set_pos(s_pop.card, x, y);
    lv_obj_set_size(s_pop.card, w, h);
    lv_obj_set_pos(s_pop.body, s_pop.to.x1 - x, s_pop.to.y1 - y);
    const std::int32_t shown = std::clamp<std::int32_t>(p * 255 / 700, 0, 255);
    lv_obj_set_style_opa(s_pop.body, static_cast<lv_opa_t>(shown), 0);
    lv_obj_set_style_opa(s_pop.scrim, static_cast<lv_opa_t>(std::clamp<std::int32_t>(p * 255 / PROGRESS, 0, 255)), 0);
}

void finish_close(lv_anim_t *)
{
    if (s_pop.source != nullptr) {
        lv_obj_remove_state(s_pop.source, LV_STATE_USER_1);
    }
    lv_obj_delete(s_pop.scrim);
    lv_obj_delete(s_pop.card);
    s_pop = Pop{};
}

void run(std::int32_t from, std::int32_t to, std::uint32_t ms, lv_anim_path_cb_t path, lv_anim_completed_cb_t done)
{
    lv_anim_delete(&s_pop, nullptr);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_pop);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_custom_exec_cb(&a, [](lv_anim_t *an, int32_t v) { grow(an, v); });
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, path);
    if (done != nullptr) {
        lv_anim_set_completed_cb(&a, done);
    }
    lv_anim_start(&a);
}

}  // namespace

bool popover_open() { return s_pop.card != nullptr && !s_pop.closing; }

lv_obj_t *popover(lv_obj_t *source, std::int32_t w, std::int32_t h, From from, bool blur_scrim)
{
    close_popover(false);
    lv_obj_t *top = lv_screen_active();  // not the top layer: glass there has nothing under it to blur
    lv_area_t src;
    lv_obj_get_coords(source, &src);

    std::int32_t x, y;
    if (from == From::Below) {
        x = std::clamp<std::int32_t>((src.x1 + src.x2) / 2 - w / 2, MARGIN, size::screen_w - MARGIN - w);
        y = src.y2 + GAP;
        if (y + h > size::screen_h - MARGIN) {
            y = size::screen_h - MARGIN - h;
        }
    } else {
        x = src.x2 + GAP;
        y = std::clamp<std::int32_t>(src.y1, MARGIN, size::screen_h - MARGIN - h);
    }
    s_pop.to     = {x, y, x + w - 1, y + h - 1};
    s_pop.from   = src;
    s_pop.source = source;
    lv_obj_add_state(source, LV_STATE_USER_1);

    s_pop.scrim = lv_obj_create(top);
    lv_obj_remove_style_all(s_pop.scrim);
    lv_obj_set_size(s_pop.scrim, size::screen_w, size::screen_h);
    lv_obj_set_style_bg_color(s_pop.scrim, lv_color_hex(colours().scrim), 0);
    lv_obj_set_style_bg_opa(s_pop.scrim, colours().scrim_opa, 0);
    if (blur_scrim) {
        lv_obj_set_style_blur_backdrop(s_pop.scrim, true, 0);
        lv_obj_set_style_blur_radius(s_pop.scrim, 6, 0);
    }
    lv_obj_add_flag(s_pop.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_pop.scrim, [](lv_event_t *) { close_popover(true); }, LV_EVENT_CLICKED, nullptr);

    s_pop.card = glass(top, Glass::Large);
    lv_obj_set_style_radius(s_pop.card, radius::pop, 0);
    lv_obj_set_style_pad_all(s_pop.card, 0, 0);
    lv_obj_add_flag(s_pop.card, LV_OBJ_FLAG_CLICKABLE);

    s_pop.body = lv_obj_create(s_pop.card);
    lv_obj_remove_style_all(s_pop.body);
    lv_obj_set_size(s_pop.body, w, h);
    lv_obj_set_style_pad_all(s_pop.body, space::xxl, 0);
    lv_obj_remove_flag(s_pop.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pop.body, LV_OBJ_FLAG_IGNORE_LAYOUT);

    grow(nullptr, 0);
    run(0, PROGRESS, motion::pop_ms, motion::spring, nullptr);
    return s_pop.body;
}

void close_popover(bool animate)
{
    if (s_pop.card == nullptr || s_pop.closing) {
        return;
    }
    if (!animate) {
        lv_anim_delete(&s_pop, nullptr);
        finish_close(nullptr);
        return;
    }
    s_pop.closing = true;
    run(PROGRESS, 0, motion::close_ms, lv_anim_path_ease_in, finish_close);
}

// --- notices --------------------------------------------------------------

namespace {
void drop(void *obj, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), y); }

void leave(lv_obj_t *slab)
{
    if (slab == nullptr || lv_obj_has_flag(slab, LV_OBJ_FLAG_USER_4)) {
        return;
    }
    lv_obj_add_flag(slab, LV_OBJ_FLAG_USER_4);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slab);
    lv_anim_set_values(&a, lv_obj_get_y(slab), -lv_obj_get_height(slab) - 40);
    lv_anim_set_exec_cb(&a, drop);
    lv_anim_set_duration(&a, motion::close_ms + 60);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, [](lv_anim_t *an) {
        auto *gone = static_cast<lv_obj_t *>(an->var);
        if (gone == s_notice) {
            s_notice = nullptr;
        }
        lv_obj_delete_async(gone);
    });
    lv_anim_start(&a);
}
}  // namespace

lv_obj_t *notice(const char *source, const char *glyph, const char *title, const char *message,
                 std::uint32_t timeout_ms)
{
    if (s_notice != nullptr) {
        leave(s_notice);
    }
    constexpr std::int32_t W = 620, H = 150, Y = 100;
    lv_obj_t *slab = glass(lv_screen_active(), Glass::Large);
    s_notice       = slab;
    lv_obj_set_style_radius(slab, radius::pop, 0);
    lv_obj_set_size(slab, W, H);
    lv_obj_set_pos(slab, (size::screen_w - W) / 2, -H);
    lv_obj_set_style_pad_all(slab, space::xl, 0);
    lv_obj_set_flex_flow(slab, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(slab, space::l, 0);
    lv_obj_add_flag(slab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slab, [](lv_event_t *e) { leave(lv_event_get_target_obj(e)); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *mark = well(slab);
    lv_obj_set_size(mark, 76, 76);
    lv_obj_set_style_pad_all(mark, 0, 0);
    lv_obj_center(icon(mark, glyph, 34, true));

    lv_obj_t *col = lv_obj_create(slab);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *head = lv_obj_create(col);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(text(head, Role::Label, source, Ink::Secondary), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(text(head, Role::Label, "09:41", Ink::Secondary), LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_pad_top(text(col, Role::H2, title), space::s, 0);
    text(col, Role::Small, message, Ink::Secondary);

    lv_obj_t *line = lv_obj_create(col);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, LV_PCT(100), 3);
    lv_obj_set_style_bg_color(line, colour(Ink::Primary), 0);
    lv_obj_set_style_bg_opa(line, colours().ink3_opa, 0);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_set_style_margin_top(line, space::m, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, slab);
    lv_anim_set_values(&a, -H, Y);
    lv_anim_set_exec_cb(&a, drop);
    lv_anim_set_duration(&a, motion::pop_ms);
    lv_anim_set_path_cb(&a, motion::spring);
    lv_anim_start(&a);

    if (timeout_ms > 0) {
        lv_anim_t t;
        lv_anim_init(&t);
        lv_anim_set_var(&t, line);
        lv_anim_set_values(&t, 100, 0);
        lv_anim_set_exec_cb(&t, [](void *obj, int32_t v) { lv_obj_set_width(static_cast<lv_obj_t *>(obj), lv_pct(v)); });
        lv_anim_set_duration(&t, timeout_ms);
        lv_anim_set_path_cb(&t, lv_anim_path_linear);
        lv_anim_set_completed_cb(&t, [](lv_anim_t *an) {
            lv_obj_t *slab_of = lv_obj_get_parent(lv_obj_get_parent(static_cast<lv_obj_t *>(an->var)));
            leave(slab_of);
        });
        lv_anim_start(&t);
    }
    return slab;
}

}  // namespace ui::kit
