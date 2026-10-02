#include "views.h"

#include "topics.h"

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

// Told as a view's buttons show and fade: the fading ones, and any view's own.
struct Watcher {
    ViewId                                view = kNoView;
    std::function<void(bool, bool)>       changed;  // shown, and whether to animate it
    bool                                  shown = true;
};

std::vector<Watcher> &watchers()
{
    static std::vector<Watcher> list;
    return list;
}

// The layered opacity, so a control's own, as faded when it cannot be used, is
// kept underneath.
void set_layer_opa(void *obj, std::int32_t opa)
{
    lv_obj_set_style_opa_layered(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(opa), 0);
}

void fade(lv_obj_t *obj, bool in, bool animate)
{
    lv_anim_delete(obj, set_layer_opa);
    if (in) {
        lv_obj_set_hidden(obj, false);
    }
    if (!animate) {
        set_layer_opa(obj, in ? LV_OPA_COVER : LV_OPA_TRANSP);
        lv_obj_set_hidden(obj, !in);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, set_layer_opa);
    lv_anim_set_values(&anim, lv_obj_get_style_opa_layered(obj, LV_PART_MAIN), in ? LV_OPA_COVER : LV_OPA_TRANSP);
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
    for (Watcher &watcher : watchers()) {
        if (view_open(watcher.view) && touched != watcher.shown) {
            watcher.shown = touched;
            watcher.changed(touched, true);
        }
    }
}

void watch(ViewId view, std::function<void(bool, bool)> changed)
{
    if (watchers().empty()) {
        lv_timer_create(check_idle, IDLE_CHECK_MS, nullptr);
    }
    watchers().push_back({view, std::move(changed), true});
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
    for (Watcher &watcher : watchers()) {
        if (watcher.view == view && !watcher.shown) {
            watcher.shown = true;
            watcher.changed(true, false);
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
    deliver_to_view(view);  // what changed while it was closed, now it is laid out
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
    if (obj != nullptr) {
        watch(view, [obj](bool shown, bool animate) { fade(obj, shown, animate); });
    }
}

void when_buttons_change(ViewId view, std::function<void(bool shown)> changed)
{
    watch(view, [changed = std::move(changed)](bool shown, bool) { changed(shown); });
}

}  // namespace ui::detail
