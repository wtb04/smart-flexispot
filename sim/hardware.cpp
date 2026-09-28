#include "hardware.h"

#include "esp_timer.h"
#include "focus_plan.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <ctime>

namespace hardware {
namespace {
// The box's presets as ours holds them, in its order, and how fast it moves.
constexpr int HEIGHTS_MM[ui::kPresetCount] = {700, 660, 1120, 740, 720, 1060};
constexpr int SPEED_MM_S                   = 38;
constexpr int AT_PRESET_MM                 = 5;
constexpr int REPORT_EVERY_MS              = 100;

int          s_height_mm  = HEIGHTS_MM[3];
int          s_heights[ui::kPresetCount];
int          s_target_mm  = -1;  // on its way to a preset
ui::Move     s_moving     = ui::Move::Stop;  // held on the rail
bool         s_linked     = true;
std::int64_t s_reported_ms = 0;

focus::State s_focus;
focus::Plan  s_plan;

struct Battery {
    bool present, charging;
    int  percent;
};
constexpr Battery BATTERIES[] = {{true, true, 100}, {true, false, 64}, {true, false, 12}, {false, false, 0}};
int  s_battery = 0;
bool s_phone   = true;
bool s_wifi    = true;
bool s_hass    = false;  // Home Assistant, as home_assistant.cpp plays it

std::int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

void show_presets()
{
    for (int i = 0; i < ui::kPresetCount; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            ui::set_preset_active(i, s_linked && std::abs(s_height_mm - s_heights[i]) <= AT_PRESET_MM));
    }
}

void show_focus()
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_focus(ui::Focus{
        .phase          = static_cast<ui::FocusPhase>(s_focus.phase),
        .round          = s_focus.round,
        .rounds         = s_plan.rounds,
        .running        = s_focus.running,
        .ends_at_ms     = s_focus.ends_at,
        .left_ms        = s_focus.left,
        .length_ms      = s_focus.length,
        .work_min       = s_plan.work_min,
        .break_min      = s_plan.break_min,
        .long_break_min = s_plan.long_break_min,
    }));
}

void move_desk(std::int64_t now)
{
    const int step = SPEED_MM_S * REPORT_EVERY_MS / 1000;
    if (s_moving != ui::Move::Stop) {
        s_height_mm += s_moving == ui::Move::Up ? step : -step;
    } else if (s_target_mm >= 0) {
        const int left = s_target_mm - s_height_mm;
        s_height_mm += std::abs(left) <= step ? left : (left > 0 ? step : -step);
        if (s_height_mm == s_target_mm) {
            s_target_mm = -1;
        }
    } else {
        return;
    }
    s_height_mm = std::clamp(s_height_mm, 660, 1310);
    s_reported_ms = now;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_height(s_height_mm));
    show_presets();
}
}  // namespace

void start()
{
    std::copy(std::begin(HEIGHTS_MM), std::end(HEIGHTS_MM), s_heights);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_desk_available(s_linked));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_height(s_height_mm));
    show_presets();
    const Battery &b = BATTERIES[s_battery];
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_battery(b.present, b.percent, b.charging));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_presence(true, s_phone, true));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_links(s_wifi, s_wifi && s_hass));
    show_focus();
}

void tick()
{
    const std::int64_t now = now_ms();
    if (s_linked && now - s_reported_ms >= REPORT_EVERY_MS) {
        move_desk(now);
    }
    if (s_focus.running && now >= s_focus.ends_at) {
        s_focus = focus::finished(s_focus, s_plan, now);
        show_focus();
    }
    static std::time_t s_shown = 0;
    if (const std::time_t t = std::time(nullptr); t != s_shown) {
        s_shown = t;
        std::tm local{};
        localtime_r(&t, &local);
        char text[8];
        std::strftime(text, sizeof(text), "%H:%M", &local);
        ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_time(text));
    }
}

void on_move(ui::Move direction)
{
    s_moving    = s_linked ? direction : ui::Move::Stop;
    s_target_mm = -1;
}

void on_preset(int index, bool store)
{
    if (!s_linked) {
        return;
    }
    if (store) {
        s_heights[index] = s_height_mm;
        show_presets();
    } else {
        s_target_mm = s_heights[index];
    }
}

void on_focus(ui::FocusAction action)
{
    const std::int64_t now = now_ms();
    switch (action) {
        case ui::FocusAction::Toggle: s_focus = focus::toggled(s_focus, s_plan, now); break;
        case ui::FocusAction::Skip:   s_focus = focus::after(s_focus, s_plan, now); break;
        case ui::FocusAction::Reset:  s_focus = focus::State{}; break;
    }
    show_focus();
}

void on_focus_plan(int work_min, int break_min, int long_break_min, int rounds)
{
    s_plan = {.work_min = work_min, .break_min = break_min, .rounds = rounds, .long_break_min = long_break_min};
    show_focus();
}

void toggle_desk_link()
{
    s_linked = !s_linked;
    s_moving = ui::Move::Stop;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_desk_available(s_linked));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_height(s_linked ? s_height_mm : -1));
    show_presets();
}

void next_battery()
{
    s_battery = (s_battery + 1) % static_cast<int>(std::size(BATTERIES));
    const Battery &b = BATTERIES[s_battery];
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_battery(b.present, b.percent, b.charging));
}

void toggle_phone()
{
    s_phone = !s_phone;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_presence(true, s_phone, true));
}

void toggle_wifi()
{
    s_wifi = !s_wifi;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_links(s_wifi, s_wifi && s_hass));
}

void set_home_assistant(bool up)
{
    s_hass = up;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_links(s_wifi, s_wifi && s_hass));
}
}  // namespace hardware
