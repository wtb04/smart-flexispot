#include "updates.h"

#include "esp_timer.h"
#include "ui.h"

#include <algorithm>

namespace updates {
namespace {
enum class Step { None, PanelArriving, PanelReady, CompanionArriving, CompanionRelaying, BothReady };

constexpr std::int64_t ARRIVAL_MS = 8000;  // each image, start to end

Step         s_step     = Step::None;
std::int64_t s_began_ms = 0;

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

void show()
{
    const bool arriving = s_step == Step::PanelArriving || s_step == Step::CompanionArriving ||
                          s_step == Step::CompanionRelaying;
    const std::int64_t gone    = now_ms() - s_began_ms;
    const int          percent = arriving ? static_cast<int>(std::min<std::int64_t>(100, gone * 100 / ARRIVAL_MS)) : 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_update({
        .busy = s_step == Step::PanelArriving                                            ? ui::UpdateTarget::Panel
                : s_step == Step::CompanionArriving || s_step == Step::CompanionRelaying ? ui::UpdateTarget::Companion
                                                                                         : ui::UpdateTarget::None,
        .phase           = s_step == Step::CompanionRelaying ? ui::UpdatePhase::Installing : ui::UpdatePhase::Receiving,
        .percent         = percent,
        .seconds_left    = arriving ? static_cast<int>((ARRIVAL_MS - gone) / 1000) : -1,
        .immediate       = false,
        .panel_ready     = s_step >= Step::PanelReady,
        .companion_ready = s_step == Step::BothReady,
    }));
}

void go(Step step)
{
    s_step     = step;
    s_began_ms = now_ms();
    show();
}
}  // namespace

void next()
{
    switch (s_step) {
        case Step::None:              go(Step::PanelArriving); break;
        case Step::PanelArriving:     go(Step::PanelReady); break;
        case Step::PanelReady:        go(Step::CompanionArriving); break;
        case Step::CompanionArriving: go(Step::CompanionRelaying); break;
        case Step::CompanionRelaying: go(Step::BothReady); break;
        case Step::BothReady:         go(Step::None); break;
    }
}

void tick()
{
    static std::int64_t s_shown_ms = 0;
    const bool arriving = s_step == Step::PanelArriving || s_step == Step::CompanionArriving ||
                          s_step == Step::CompanionRelaying;
    if (!arriving || now_ms() - s_shown_ms < 250) {
        return;
    }
    s_shown_ms = now_ms();
    if (now_ms() - s_began_ms >= ARRIVAL_MS) {
        next();  // in: ready, or on to the companion
    } else {
        show();
    }
}

void install()
{
    if (s_step == Step::PanelReady || s_step == Step::BothReady) {
        go(Step::None);
    }
}
}  // namespace updates
