#include "hardware.h"

#include "esp_timer.h"
#include "focus_plan.h"
#include "ical.h"
#include "radar.h"

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
std::int64_t s_stopped_ms = -1'000'000;  // when a key last stopped it
constexpr std::int64_t BOX_REST_MS = 1200;  // measured on the desk
ui::Move     s_moving     = ui::Move::Stop;  // held in the desk fold-out
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
    // As the panel keeps them, while Setup shows them.
    constexpr std::int64_t DIAGNOSTICS_EVERY_MS = 2000;
    static std::int64_t    s_diagnosed_ms       = 0;
    if (ui::diagnostics_open() && now_ms() - s_diagnosed_ms >= DIAGNOSTICS_EVERY_MS) {
        s_diagnosed_ms = now_ms();
        diagnostics();
    }
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
    } else if (s_target_mm >= 0 || now_ms() - s_stopped_ms < BOX_REST_MS) {
        // As the box: any key while the desk moves stops it, and for a while after
        // it stopped, a start is taken as one more stop.
        std::printf("desk: preset %d pressed while moving or just stopped, stops\n", index + 1);
        if (s_target_mm >= 0) {
            s_stopped_ms = now_ms();
        }
        s_target_mm = -1;
    } else {
        std::printf("desk: preset %d pressed, goes to %d mm\n", index + 1, s_heights[index]);
        s_target_mm = s_heights[index];
        for (int i = 0; i < ui::kPresetCount; ++i) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_preset_active(i, false));  // off it, as the desk says once told to go
        }
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

namespace {
// The cards in the order sim/main.cpp lays them out, and their rows.
enum Card { WIFI, HASS, PRESENCE, DESK, LINK, POWER, RADAR, CALENDAR, SYSTEM };

void card(Card card, const char *summary, ui::Level level)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_card(card, summary, level));
}

void row(Card card, int row, const char *value, ui::Level level = ui::Level::Neutral)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_row(card, row, value, level));
}

int preset_at()
{
    for (int i = 0; i < ui::kPresetCount; ++i) {
        if (s_heights[i] >= 0 && std::abs(s_height_mm - s_heights[i]) <= AT_PRESET_MM) {
            return i;
        }
    }
    return -1;
}
}  // namespace

void diagnostics()
{
    using ui::Level;
    char text[48];

    constexpr int SIGNAL_DBM = -52;
    std::snprintf(text, sizeof(text), "%d dBm", SIGNAL_DBM);
    card(WIFI, s_wifi ? text : "offline", s_wifi ? Level::Good : Level::Bad);
    row(WIFI, 0, s_wifi ? "the simulator's" : "not joined", s_wifi ? Level::Neutral : Level::Bad);
    row(WIFI, 1, s_wifi ? text : nullptr, Level::Good);
    row(WIFI, 2, s_wifi ? "127.0.0.1" : nullptr);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::Wifi, s_wifi ? text : "offline"));

    card(HASS, s_hass ? "connected" : "offline", s_hass ? Level::Good : Level::Bad);
    row(HASS, 0, s_hass ? "connected" : "offline", s_hass ? Level::Good : Level::Bad);
    row(HASS, 1, s_hass ? "connected" : "offline", s_hass ? Level::Good : Level::Bad);

    card(PRESENCE, s_phone ? "home" : "away", Level::Good);
    row(PRESENCE, 0, s_phone ? "home" : "away", Level::Good);
    row(PRESENCE, 2, "set", Level::Good);

    std::snprintf(text, sizeof(text), "%d.%d cm", s_height_mm / 10, s_height_mm % 10);
    card(DESK, s_linked ? text : "--", s_linked ? Level::Good : Level::Neutral);
    row(DESK, 0, s_linked ? text : nullptr);
    row(DESK, 1, ui::preset_name(preset_at()));
    row(DESK, 2, s_target_mm >= 0 || s_moving != ui::Move::Stop ? "moving" : "still");

    card(LINK, s_linked ? "wire" : "box silent", s_linked ? Level::Good : Level::Bad);
    row(LINK, 0, "wire");
    row(LINK, 1, s_linked ? "answering" : "silent", s_linked ? Level::Good : Level::Bad);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::DeskLink, s_linked ? "wire" : nullptr));

    const Battery &battery = BATTERIES[s_battery];
    if (!battery.present) {
        card(POWER, "no battery", Level::Neutral);
        row(POWER, 0, "cable");
    } else {
        std::snprintf(text, sizeof(text), "%d%%", battery.percent);
        const Level charge = battery.percent >= 50 ? Level::Good : battery.percent >= 20 ? Level::Warn : Level::Bad;
        card(POWER, text, charge);
        row(POWER, 0, battery.on_battery ? "battery" : "cable");
        row(POWER, 1, text, charge);
        row(POWER, 4, battery.charging ? "charging" : battery.on_battery ? "discharging" : "full");
    }

    static radar::Snapshot sky;
    radar::snapshot(sky);
    if (sky.age_s < 0) {
        card(RADAR, "--", Level::Neutral);
    } else {
        std::snprintf(text, sizeof(text), "%d plane%s", sky.count, sky.count == 1 ? "" : "s");
        card(RADAR, text, sky.ok ? Level::Good : Level::Warn);
        row(RADAR, 0, sky.ok ? "answering" : "refusing", sky.ok ? Level::Good : Level::Warn);
        std::snprintf(text, sizeof(text), "%d", sky.count);
        row(RADAR, 1, text);
        std::snprintf(text, sizeof(text), "%d km", sky.range_km);
        row(RADAR, 2, text);
        std::snprintf(text, sizeof(text), "%d s ago", sky.age_s);
        row(RADAR, 3, text);
    }

    static ical::Event ahead[8];
    const int          events = ical::upcoming(ahead, static_cast<int>(std::size(ahead)));
    std::snprintf(text, sizeof(text), "%d", ical::kFeedCount);
    row(CALENDAR, 0, text);
    if (events == 0) {
        card(CALENDAR, "nothing ahead", Level::Neutral);
        row(CALENDAR, 1, "nothing");
    } else {
        std::tm    local{};
        const auto when = static_cast<std::time_t>(ahead[0].start);
        localtime_r(&when, &local);
        std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
        card(CALENDAR, text, Level::Good);
        row(CALENDAR, 2, text);
        std::snprintf(text, sizeof(text), "%d event%s", events, events == 1 ? "" : "s");
        row(CALENDAR, 1, text);
    }

    const auto up = static_cast<unsigned>(now_ms() / 1000);
    std::snprintf(text, sizeof(text), "%um %us", up / 60, up % 60);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_glance(ui::Glance::Uptime, text));
    card(SYSTEM, "simulator", Level::Good);
    row(SYSTEM, 0, "simulator");
    row(SYSTEM, 2, text);
}

void set_home_assistant(bool up)
{
    s_hass = up;
    ESP_ERROR_CHECK_WITHOUT_ABORT(ui::set_links(s_wifi, s_wifi && s_hass));
}
}  // namespace hardware
