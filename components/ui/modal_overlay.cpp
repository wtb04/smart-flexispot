#include "modal_overlay.h"

#include "theme.h"

namespace ui {
namespace {

constexpr lv_opa_t     SCRIM_OPA = LV_OPA_70;
constexpr std::int32_t SLIDE     = 56;
constexpr std::uint32_t OPEN_MS  = 220;
constexpr std::uint32_t CLOSE_MS = 150;

constexpr std::int32_t PROGRESS_MAX = 255;

}  // namespace

ModalOverlay::ModalOverlay(lv_obj_t *parent, std::int32_t width, std::int32_t height)
    : width_(width), height_(height)
{
    scrim_ = lv_obj_create(parent);
    lv_obj_set_align(scrim_, LV_ALIGN_TOP_LEFT);
    // Sized in open(): a percentage covers only the parent's content box, leaving its padding
    // undimmed -- which showed as a bright border around a dimmed page.
    theme::style_panel(scrim_, theme::background, 0);
    // Not animated: fading a scrim over the page means re-blending all of it every frame.
    lv_obj_set_style_bg_opa(scrim_, SCRIM_OPA, 0);
    lv_obj_set_hidden(scrim_, true);
    lv_obj_set_clickable(scrim_, true);
    lv_obj_add_event_cb(scrim_, scrim_clicked, LV_EVENT_CLICKED, this);

    card_ = lv_obj_create(scrim_);
    lv_obj_set_size(card_, width_, height_);
    // set_pos is interpreted relative to the alignment, so a sliding card must not stay centred.
    lv_obj_set_align(card_, LV_ALIGN_TOP_LEFT);
    theme::style_panel(card_, theme::panel, 24);
    // Without this a tap on the card bubbles to the scrim and closes it.
    lv_obj_set_clickable(card_, true);
}

void ModalOverlay::open(lv_obj_t *)
{
    lv_obj_update_layout(scrim_);

    // Children sit in the parent's content box, so backing out by its padding reaches the edge.
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
    visible_ = true;
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

// Position only: the card keeps its size, so its contents are laid out once rather than per frame.
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
