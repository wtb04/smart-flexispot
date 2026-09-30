#include "views.h"

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

}  // namespace ui::detail
