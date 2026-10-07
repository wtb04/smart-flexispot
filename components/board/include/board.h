#pragma once

#include "lvgl.h"

#include "esp_err.h"

#include <cstdint>

namespace board {
/** The backlight off and held so, before anything else, and until the first
 *  brightness is set: the panel shows flat blue while it has no picture. */
void dark_from_the_start();

/** The same, for a restart or a crash to leave behind. Safe in the panic handler. */
void hold_dark();

/** A restart the chip comes out of as from power on, as a plain one does not:
 *  the Wi-Fi co-processor switched off, then all of the chip reset, its pins
 *  let go, by its own watchdog. The co-processor has come back from a plain
 *  restart out of reach. Does not return. */
[[noreturn]] void restart_cold();

/** Brings up rails, panel, touch and the LVGL port task. Once, before any lv_*
 *  call. flipped hangs the panel the other way up. */
esp_err_t init(bool flipped);

/** Turns the picture and the touchscreen over together. From any task. */
void set_flipped(bool flipped);

/** Panel backlight, 0-100. */
esp_err_t set_brightness(int percent);

/** Shaped for ui::BrightnessHandler, which cannot report errors. */
void set_brightness_percent(int percent);

/** Lights the backlight. Kept out of init() so the panel stays dark until
 *  there is something on it worth seeing. */
esp_err_t display_on(int percent);

/** Backlight off and the panel asleep. The panel goes on scanning out whatever
 *  the MIPI link last left it, so anything that ends the program -- a restart,
 *  above all -- has to put it to sleep rather than only dim it. */
esp_err_t display_off();

/** How many frames the panel has been sent since it started, some 58 a
 *  second; fewer when something held its interrupt off. */
std::uint32_t refreshes();

/** Times the panel's frame interrupt came late: past about seven frames, the
 *  ring its DMA goes round by itself runs out, and the panel flickers blue.
 *  How many since it started, and the latest's gap and uptime. */
struct LateFrames {
    std::uint32_t count;
    std::int32_t  gap_us;
    std::int64_t  at_us;
};
LateFrames late_frames();

/** How many times the panel's reads have fallen behind since it started: each
 *  a moment of blue screen, which the DSI driver reports only on the console. */
std::uint32_t underruns();

/** Where the time putting LVGL's areas on the panel went since the last call:
 *  turning them onto it, waiting for the panel to let go of the buffer, and
 *  bringing that buffer up to date first. For a development build's bench. */
struct FlushTimes {
    std::uint32_t frames      = 0;
    std::uint32_t areas       = 0;
    std::uint64_t pixels      = 0;
    std::int64_t  rotate_us   = 0;
    std::int64_t  wait_us     = 0;
    std::int64_t  catch_up_us = 0;
};
FlushTimes take_flush_times();

/** A zoom drawn past LVGL, which is to draw nothing until zoom_end(): each
 *  frame is the picture LVGL shows, magnified by the PPA as it turns it onto
 *  the panel, where a whole frame drawn by LVGL was 88 ms. zoom_begin() keeps
 *  `keep`, the controls over the picture, as the frame on show has them, and
 *  each frame puts them back over it. Areas are in LVGL's screen coordinates. */
struct ZoomFrame {
    const std::uint16_t *picture = nullptr;  // RGB565, as LVGL shows it
    std::int32_t         w = 0, h = 0;
    std::int32_t         x = 0, y = 0;       // its top left on the screen
    float                cx = 0, cy = 0;     // the point in it that stays put
    float                scale = 1.0f;       // at least 1, in steps of 1/16
    lv_area_t            to{};               // what of the screen it fills, within it
    // Drawn where they are in the picture, unmagnified, over what is: the
    // scope's rings, which stay as the map grows under them. Indices into the
    // picture, each with its opacity, in one colour.
    const std::uint32_t *ring_at    = nullptr;
    const std::uint8_t  *ring_opa   = nullptr;
    std::size_t          ring_count = 0;
    std::uint16_t        ring_ink   = 0;
};
esp_err_t zoom_begin(const lv_area_t *keep, int count);

/** The frame on show, turned back the way LVGL draws it and shrunk by the PPA
 *  to `w` by `h` into `out` (RGB565, cache-line aligned, `out_bytes` long): a
 *  picture of the screen as it is, for a blurred backdrop, without drawing it
 *  again. The shrink is in steps of 1/16. */
esp_err_t shrink_shown(std::uint8_t *out, std::size_t out_bytes, std::uint32_t w, std::uint32_t h);
esp_err_t zoom_frame(const ZoomFrame &frame);
void      zoom_end();

/** A page glided across past LVGL: glide_begin() keeps the frame on show,
 *  the old page, and holds back the next frame LVGL draws, the new page at
 *  rest; each step then lays the two side by side, the new one
 *  `shift` pixels in from the right of `region`, or the left, by copying
 *  rather than drawing; glide_end() shows the held frame. False where the
 *  panel cannot. */
bool glide_begin(const lv_area_t &region, bool from_left);
bool glide_held();
/** A step's frame laid down, LVGL then to draw what changed round the region
 *  into it, the dock's mark sliding, before glide_show() puts it on show. */
bool glide_compose(std::int32_t shift);
void glide_show();
void glide_end();
struct GlideTimes {
    std::uint32_t frames  = 0;
    std::int64_t  copy_us = 0;
    std::int64_t  wait_us = 0;  // of copy_us, waiting for a buffer the panel is done with
    std::int64_t  show_us = 0;  // LVGL's own drawing round the region, and the frame put on show
};
GlideTimes glide_times();

/** The frame the panel is showing, as the panel holds it: portrait, `w` by `h`
 *  RGB565, for a development build to look at exactly what is on the glass. */
const std::uint16_t *shown_frame(int *w, int *h);
/** Done with it: the panel may draw into it again. */
void let_go_of_frame();

/** Below this the panel does not get any dimmer, so offering the range is just a
 *  control that appears broken. */
inline constexpr int kMinBrightness = 20;

}  // namespace board
