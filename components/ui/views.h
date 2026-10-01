#pragma once

#include "lvgl.h"

#include <cstdint>
#include <functional>

// Everything that opens over the pages, in one place: the fullscreen views
// (radar, focus, cinema) and the popups. Each gives its root and what to do as
// it opens and closes; this shows, hides and stacks them, keeps one fullscreen
// view at a time, and answers what is open. On the LVGL task only.
namespace ui::detail {

enum class ViewKind : std::uint8_t {
    Fullscreen,  // over the whole screen, the rail and tabs too
    Popup,       // a card on a scrim over part of it
};

struct ViewSpec {
    const char           *name = "";
    ViewKind              kind = ViewKind::Popup;
    lv_obj_t             *root = nullptr;  // hidden until opened
    std::function<void()> opened;          // after it is shown and on top: lay out, start timers
    std::function<void()> closed;          // after it is hidden: stop them
};

using ViewId                    = int;
inline constexpr ViewId kNoView = -1;

ViewId add_view(ViewSpec spec);

/** Shown and on top; opening a fullscreen view closes any other. */
void open_view(ViewId view);
void close_view(ViewId view);
void toggle_view(ViewId view);
bool view_open(ViewId view);

/** Whether a fullscreen view covers the screen, for what places itself round it. */
bool fullscreen_open();

/** Fades `obj`, a control over `view`, out once the screen has gone untouched
 *  for ten seconds while the view is open, and in again at the next touch;
 *  whole again as the view closes. Faded out, it is hidden, so a tap there goes
 *  to what is under it. */
void fade_when_idle(ViewId view, lv_obj_t *obj);

/** Tells `changed` as the buttons over `view` fade out (false) and come back
 *  (true), and true as it closes, for what a view does more than fade. */
void when_buttons_change(ViewId view, std::function<void(bool shown)> changed);

}  // namespace ui::detail
