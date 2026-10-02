#include "media_model.h"

#include "freertos/task.h"
#include "units.h"

#include <algorithm>

namespace ui::detail {

MediaState &media_state()
{
    static MediaState state;
    return state;
}

int media_position_now()
{
    return media_position_ms_now() / units::kMsPerSecond;
}

int media_position_ms_now()
{
    const MediaState &media = media_state();
    std::int64_t      at    = static_cast<std::int64_t>(media.position_s) * units::kMsPerSecond;
    if (media.advancing) {
        at += static_cast<std::int64_t>(xTaskGetTickCount() - media.position_at) * units::kMsPerSecond /
              configTICK_RATE_HZ;
    }
    const std::int64_t end = static_cast<std::int64_t>(media.duration_s) * units::kMsPerSecond;
    return static_cast<int>(end > 0 ? std::min(at, end) : at);
}

bool media_is_video()
{
    return media_state().video && media_state().has_track;
}

}  // namespace ui::detail
