#pragma once

#include <functional>

// What the whole panel goes by, said once by whoever knows it and heard by
// whoever cares, rather than passed along by hand to each in turn.
//
//     app::watch(app::Fact::ScreenOn, [](bool on) { set_dark(!on); });
namespace app {

enum class Fact : int {
    ScreenOn,  // the backlight is on; set by whoever turns it on or off
    Online,    // Wi-Fi has an address; set by wifi
};

bool get(Fact fact);

/** Tells every watcher, on the caller's task, when it changes. */
void set(Fact fact, bool value);

/** Called with each change from then on; quick, as it holds up whoever set
 *  it. Watchers are added at start and never removed. */
void watch(Fact fact, std::function<void(bool)> on_change);

}  // namespace app
