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
// The last two are the panel's own, empty until held to save one.
constexpr int HEIGHTS_MM[ui::kPresetCount] = {700, 660, 1120, 740, -1, -1};
constexpr int SAVED_NOTICE_MS = 2500;  // as components/desk has them
constexpr int HINT_NOTICE_MS  = 3000;
constexpr int FOCUS_NOTICE_MS = 5000;  // as main.cpp has it
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
    bool present, charging, on_battery;
    int  percent;
};
// Plugged in and charging, unplugged, unplugged and low, and no pack at all.
constexpr Battery BATTERIES[] = {{true, true, false, 100}, {true, false, true, 64}, {true, false, true, 12}, {false, false, false, 0}};
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
            ui::set_preset_active(i, s_linked && s_heights[i] >= 0 && std::abs(s_height_mm - s_heights[i]) <= AT_PRESET_MM));
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

void desk_notice(const char *message, bool done, int timeout_ms)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        ui::notify("Desk", "", message, done ? ui::Level::Good : ui::Level::Neutral, timeout_ms));
}

// As main.cpp's on_focus_change says a part ran out.
void focus_notice()
{
    char title[64], message[64];
    switch (s_focus.phase) {
        case focus::Phase::Break:
            std::snprintf(title, sizeof(title), "Round %d done", s_focus.round);
            std::snprintf(message, sizeof(message), "A %d min break is ready", s_plan.break_min);
            break;
        case focus::Phase::LongBreak:
            std::snprintf(title, sizeof(title), "All %d rounds done", s_plan.rounds);
            std::snprintf(message, sizeof(message), "A %d min break is ready", s_plan.long_break_min);
            break;
        case focus::Phase::Work:
            std::snprintf(title, sizeof(title), "Break over");
            std::snprintf(message, sizeof(message), "Round %d of %d is ready", s_focus.round, s_plan.rounds);
            break;
        case focus::Phase::Idle:
            return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::notify("Focus", title, message, ui::Level::Good, FOCUS_NOTICE_MS));
}

void finish_part(std::int64_t now)
{
    s_focus = focus::finished(s_focus, s_plan, now);
    show_focus();
    focus_notice();
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_battery(b.present, b.percent, b.charging, b.on_battery));
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
        finish_part(now);
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
        desk_notice("Preset saved", true, SAVED_NOTICE_MS);
    } else if (s_heights[index] < 0) {
        desk_notice("Hold it to save the height it goes to", false, HINT_NOTICE_MS);
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

void end_focus_part()
{
    if (s_focus.phase != focus::Phase::Idle) {
        finish_part(now_ms());
    }
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
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_battery(b.present, b.percent, b.charging, b.on_battery));
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
