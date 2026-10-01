#include "views.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace ui::detail {
namespace {
struct Entry {
    ViewSpec spec;
    bool     open = false;
};

std::vector<Entry> &views()
{
    static std::vector<Entry> list;
    return list;
}

Entry *find(ViewId view)
{
    return view >= 0 && view < static_cast<ViewId>(views().size()) ? &views()[view] : nullptr;
}

constexpr std::uint32_t IDLE_AFTER_MS = 10 * 1000;
constexpr std::uint32_t IDLE_FADE_MS  = 300;
constexpr std::uint32_t IDLE_CHECK_MS = 200;

struct Floating {
    ViewId    view  = kNoView;
    lv_obj_t *obj   = nullptr;
    bool      shown = true;
};

std::vector<Floating> &floating()
{
    static std::vector<Floating> list;
    return list;
}

// The layered opacity, so a control's own, as faded when it cannot be used, is
// kept underneath.
void set_layer_opa(void *obj, std::int32_t opa)
{
    lv_obj_set_style_opa_layered(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(opa), 0);
}

void fade(Floating &item, bool in, bool animate)
{
    item.shown = in;
    lv_anim_delete(item.obj, set_layer_opa);
    if (in) {
        lv_obj_set_hidden(item.obj, false);
    }
    if (!animate) {
        set_layer_opa(item.obj, in ? LV_OPA_COVER : LV_OPA_TRANSP);
        lv_obj_set_hidden(item.obj, !in);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, item.obj);
    lv_anim_set_exec_cb(&anim, set_layer_opa);
    lv_anim_set_values(&anim, lv_obj_get_style_opa_layered(item.obj, LV_PART_MAIN), in ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_anim_set_duration(&anim, IDLE_FADE_MS);
    if (!in) {
        lv_anim_set_completed_cb(&anim, [](lv_anim_t *done) {
            lv_obj_set_hidden(static_cast<lv_obj_t *>(done->var), true);
        });
    }
    lv_anim_start(&anim);
}

void check_idle(lv_timer_t *)
{
    const bool touched = lv_display_get_inactive_time(nullptr) < IDLE_AFTER_MS;
    for (Floating &item : floating()) {
        if (view_open(item.view) && touched != item.shown) {
            fade(item, touched, true);
        }
    }
}
}  // namespace

ViewId add_view(ViewSpec spec)
{
    if (spec.root != nullptr) {
        lv_obj_set_hidden(spec.root, true);
    }
    views().push_back({std::move(spec), false});
    return static_cast<ViewId>(views().size() - 1);
}

void close_view(ViewId view)
{
    Entry *entry = find(view);
    if (entry == nullptr || !entry->open) {
        return;
    }
    entry->open = false;
    lv_obj_set_hidden(entry->spec.root, true);
    for (Floating &item : floating()) {
        if (item.view == view && !item.shown) {
            fade(item, true, false);
        }
    }
    if (entry->spec.closed) {
        entry->spec.closed();
    }
}

void open_view(ViewId view)
{
    Entry *entry = find(view);
    if (entry == nullptr || entry->spec.root == nullptr) {
        return;
    }
    if (entry->spec.kind == ViewKind::Fullscreen) {
        for (ViewId other = 0; other < static_cast<ViewId>(views().size()); ++other) {
            if (other != view && views()[other].spec.kind == ViewKind::Fullscreen) {
                close_view(other);
            }
        }
    }
    entry->open = true;
    lv_obj_set_hidden(entry->spec.root, false);
    lv_obj_move_foreground(entry->spec.root);
    if (entry->spec.opened) {
        entry->spec.opened();
    }
}

void toggle_view(ViewId view)
{
    if (view_open(view)) {
        close_view(view);
    } else {
        open_view(view);
    }
}

bool view_open(ViewId view)
{
    const Entry *entry = find(view);
    return entry != nullptr && entry->open;
}

bool fullscreen_open()
{
    for (const Entry &entry : views()) {
        if (entry.open && entry.spec.kind == ViewKind::Fullscreen) {
            return true;
        }
    }
    return false;
}

void fade_when_idle(ViewId view, lv_obj_t *obj)
{
    if (obj == nullptr) {
        return;
    }
    if (floating().empty()) {
        lv_timer_create(check_idle, IDLE_CHECK_MS, nullptr);
    }
    floating().push_back({view, obj, true});
}

}  // namespace ui::detail
