#pragma once

// The feeds the panel reads from the internet, fetched for real: the calendar,
// the way there and the sky. Each runs on a thread of its own, as on the panel,
// and pump() hands what came in to the screen on the main thread.
namespace live {
void start();

/** Once per frame, on the thread LVGL runs on. */
void pump();

/** The radar page on screen: its feed polled fast, as the panel does. */
void set_radar_showing(bool showing);

/** A plane tapped on the radar: who it is, where it flies, and its photo. */
void look_up(const char *hex, const char *callsign);

/** Starts net, which every fetch goes through; before any thread starts. */
void init_http();
}  // namespace live
