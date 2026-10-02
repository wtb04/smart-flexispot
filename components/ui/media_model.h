#pragma once

#include "freertos/FreeRTOS.h"

#include <cstdint>

// What plays, as the media card shows it: its words once its cover has come,
// whether it plays once a pause has settled, how far it is and how loud. The
// card writes it; the music and cinema views follow it. Changing it is told by
// publishing Topic::Media. On the LVGL task only.
namespace ui::detail {

struct MediaState {
    char source[32] = "";
    char title[128] = "";   // empty while nothing plays
    char artist[128] = "";  // for a video, the series, a newline, and its season and episode
    bool has_track  = false;
    bool playing    = false;  // as shown: a pause shows once it has settled
    bool remote     = true;   // takes play, pause, skip and seek from here
    bool video      = false;  // it seeks, as a film or an episode does
    int  position_s = 0;      // as last reported, at position_at
    int  duration_s = 0;      // 0 while unknown
    bool advancing  = false;  // the position runs on from position_at
    TickType_t position_at = 0;
    int  volume     = -1;     // percent, as last reported or set, -1 before either
    const void *art   = nullptr;  // the card's cover, an lv_image_dsc_t, or null without one
    const void *large = nullptr;  // the same cover at media::kLargeArtSize, pixels, or null
    std::uint32_t covers = 0;     // counts every cover that came, as one may come into the same buffer
};

/** Read anywhere; written by what applies the updates, which then publishes Topic::Media. */
MediaState &media_state();

/** Seconds, and milliseconds, into what plays, carried on while it plays. */
int  media_position_now();
int  media_position_ms_now();
bool media_is_video();

}  // namespace ui::detail
