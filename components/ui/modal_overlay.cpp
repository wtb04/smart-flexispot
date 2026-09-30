#include "modal_overlay.h"

#include "theme.h"

namespace ui {
namespace {
constexpr lv_opa_t     SCRIM_OPA   = LV_OPA_70;
constexpr std::int32_t CARD_RADIUS = 24;

constexpr std::int32_t CLOSE_SIZE = 48;
constexpr std::int32_t CLOSE_LIFT = 8;

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
    theme::style_panel(card_, theme::panel, CARD_RADIUS);
    lv_obj_set_clickable(card_, true);

    view_ = detail::add_view({"popup", detail::ViewKind::Popup, scrim_, [this] { centre(); }, nullptr});
}

void ModalOverlay::add_close_button()
{
    lv_obj_t *close = lv_button_create(card_);
    lv_obj_set_size(close, CLOSE_SIZE, CLOSE_SIZE);
    lv_obj_set_align(close, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(close, 0, -CLOSE_LIFT);
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

// Shown and hidden at once: behind the scrim the whole page is drawn again for
// every frame, some 70 ms of them here, so a slide could only stutter.
void ModalOverlay::open(lv_obj_t *)
{
    detail::open_view(view_);
}

void ModalOverlay::centre()
{
    lv_obj_update_layout(scrim_);

    lv_obj_t *parent = lv_obj_get_parent(scrim_);
    lv_obj_set_pos(scrim_, -lv_obj_get_style_pad_left(parent, LV_PART_MAIN),
                   -lv_obj_get_style_pad_top(parent, LV_PART_MAIN));
    lv_obj_set_size(scrim_, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_update_layout(scrim_);

    lv_obj_set_pos(card_, (lv_obj_get_width(scrim_) - width_) / 2,
                   (lv_obj_get_height(scrim_) - height_) / 2);
}

void ModalOverlay::close()
{
    detail::close_view(view_);
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
