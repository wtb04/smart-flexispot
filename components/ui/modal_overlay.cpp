#include "modal_overlay.h"

#include "theme.h"

namespace ui {
namespace {
constexpr lv_opa_t     SCRIM_OPA = LV_OPA_70;
constexpr std::int32_t  SLIDE    = 48;
constexpr std::uint32_t OPEN_MS  = 240;
constexpr std::uint32_t CLOSE_MS = 160;

constexpr std::int32_t CLOSE_SIZE = 48;

constexpr std::int32_t PROGRESS_MAX = 255;

}  // namespace

ModalOverlay::ModalOverlay(lv_obj_t *parent, std::int32_t width, std::int32_t height)
    : width_(width), height_(height)
{
    scrim_ = lv_obj_create(parent);
    lv_obj_set_align(scrim_, LV_ALIGN_TOP_LEFT);
    theme::style_panel(scrim_, theme::background, 0);
    lv_obj_set_style_bg_opa(scrim_, SCRIM_OPA, 0);
    lv_obj_set_hidden(scrim_, true);
    lv_obj_set_clickable(scrim_, true);
    lv_obj_add_event_cb(scrim_, scrim_clicked, LV_EVENT_CLICKED, this);

    card_ = lv_obj_create(scrim_);
    lv_obj_set_size(card_, width_, height_);
    lv_obj_set_align(card_, LV_ALIGN_TOP_LEFT);
    theme::style_panel(card_, theme::panel, 24);
    lv_obj_set_clickable(card_, true);
}

void ModalOverlay::add_close_button()
{
    lv_obj_t *close = lv_button_create(card_);
    lv_obj_set_size(close, CLOSE_SIZE, CLOSE_SIZE);
    lv_obj_set_align(close, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(close, 0, -8);
    theme::style_button(close, theme::panel_light);
    lv_obj_center(theme::make_label(close, LV_SYMBOL_CLOSE, theme::text, fonts::size_22()));
    lv_obj_add_event_cb(close, close_clicked, LV_EVENT_CLICKED, this);
    lv_obj_move_foreground(close);
}

void ModalOverlay::resize(std::int32_t width, std::int32_t height)
{
    width_  = width;
    height_ = height;
    lv_obj_set_size(card_, width_, height_);
}

std::int32_t ModalOverlay::header_height()
{
    return CLOSE_SIZE;
}

void ModalOverlay::close_clicked(lv_event_t *event)
{
    static_cast<ModalOverlay *>(lv_event_get_user_data(event))->close();
}

void ModalOverlay::open(lv_obj_t *)
{
    lv_obj_update_layout(scrim_);

    lv_obj_t *parent = lv_obj_get_parent(scrim_);
    lv_obj_set_pos(scrim_, -lv_obj_get_style_pad_left(parent, LV_PART_MAIN),
                   -lv_obj_get_style_pad_top(parent, LV_PART_MAIN));
    lv_obj_set_size(scrim_, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_update_layout(scrim_);

    lv_obj_set_x(card_, (lv_obj_get_width(scrim_) - width_) / 2);
    rest_y_ = (lv_obj_get_height(scrim_) - height_) / 2;

    lv_anim_delete(this, nullptr);
    lv_obj_set_hidden(scrim_, false);
    lv_obj_move_foreground(scrim_);
    slide(this, 0);
    visible_ = true;
    // The first frame covers the whole page, far slower than the card's own:
    // drawn before the slide starts, it does not eat the first frames of it.
    lv_refr_now(nullptr);
    start(0, PROGRESS_MAX, OPEN_MS, false);
}

void ModalOverlay::close()
{
    if (!visible_) {
        return;
    }
    visible_ = false;
    lv_anim_delete(this, nullptr);
    start(PROGRESS_MAX, 0, CLOSE_MS, true);
}

void ModalOverlay::start(std::int32_t from, std::int32_t to, std::uint32_t duration,
                         bool hide_at_end)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_exec_cb(&anim, slide);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, hide_at_end ? lv_anim_path_ease_in : lv_anim_path_ease_out);
    if (hide_at_end) {
        lv_anim_set_completed_cb(&anim, hide_when_done);
    }
    lv_anim_start(&anim);
}

void ModalOverlay::slide(void *target, std::int32_t value)
{
    auto *self = static_cast<ModalOverlay *>(target);
    lv_obj_set_y(self->card_, self->rest_y_ + SLIDE * (PROGRESS_MAX - value) / PROGRESS_MAX);
}

void ModalOverlay::hide_when_done(lv_anim_t *anim)
{
    lv_obj_set_hidden(static_cast<ModalOverlay *>(anim->var)->scrim_, true);
}

void ModalOverlay::scrim_clicked(lv_event_t *event)
{
    auto *self = static_cast<ModalOverlay *>(lv_event_get_user_data(event));
    if (lv_event_get_target_obj(event) != self->scrim_) {
        return;
    }
    self->close();
}

}  // namespace ui
