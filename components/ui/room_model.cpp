#include "room_model.h"

#include "topics.h"
#include "ui_internal.h"

#include <cstdio>

namespace ui::detail {
namespace {
// A travel that has not moved the desk for this long has ended short of its
// preset, stopped at the desk or by another button: no longer shown as going.
constexpr std::uint32_t STILL_MS       = 2500;
constexpr std::uint32_t STILL_CHECK_MS = 500;

// The box takes any key pressed while the desk moves as a stop, so a preset
// tapped while it still coasts, from a stop or on its way elsewhere, waits
// until the desk has stood still this long and is pressed then.
constexpr std::uint32_t SETTLE_MS       = 600;
constexpr std::uint32_t SETTLE_CHECK_MS = 100;

DeskState   s_desk;
LightsState s_lights;
std::uint32_t s_moved_at   = 0;  // when the height last changed, or the travel began
std::uint32_t s_height_at  = 0;  // when the height last changed
lv_timer_t   *s_still_timer = nullptr;
int           s_held        = -1;  // the preset waiting for the desk to stand still
lv_timer_t   *s_held_timer  = nullptr;

void copy(char *to, std::size_t size, const char *from)
{
    std::snprintf(to, size, "%s", from != nullptr ? from : "");
}

void stop_watching()
{
    if (s_still_timer != nullptr) {
        lv_timer_delete(s_still_timer);
        s_still_timer = nullptr;
    }
}

void forget_held()
{
    s_held = -1;
    if (s_held_timer != nullptr) {
        lv_timer_delete(s_held_timer);
        s_held_timer = nullptr;
    }
}

void arrived()
{
    stop_watching();
    forget_held();
    s_desk.travelling = -1;
    publish(Topic::Desk);
}

void check_still(lv_timer_t *)
{
    if (lv_tick_elaps(s_moved_at) >= STILL_MS) {
        arrived();
    }
}

void press(int index)
{
    if (s_handlers.preset != nullptr) {
        s_handlers.preset(index, false);
    }
}

void press_held(lv_timer_t *)
{
    if (lv_tick_elaps(s_height_at) < SETTLE_MS) {
        return;
    }
    const int index = s_held;
    forget_held();
    if (index >= 0 && index == s_desk.travelling) {
        s_moved_at = lv_tick_get();  // its travel starts now
        press(index);
    }
}

bool coasting()
{
    return s_height_at != 0 && lv_tick_elaps(s_height_at) < SETTLE_MS;
}
}  // namespace

const DeskState &desk_state()
{
    return s_desk;
}

const LightsState &lights_state()
{
    return s_lights;
}

void desk_take_active(int index, bool active)
{
    s_desk.preset_active[index] = active;
    if (active && index == s_desk.travelling) {
        arrived();
        return;
    }
    publish(Topic::Desk);
}

void desk_take_height(int height_mm)
{
    if (height_mm != s_desk.height_mm) {
        s_moved_at  = lv_tick_get();
        s_height_at = s_moved_at;
    }
    s_desk.height_mm = height_mm;
    publish(Topic::Desk);
}

void desk_take_available(bool available)
{
    s_desk.available = available;
    if (!available && s_desk.travelling >= 0) {
        arrived();
        return;
    }
    publish(Topic::Desk);
}

void desk_go_to(int index)
{
    if (index < 0 || index >= kPresetCount || s_desk.preset_active[index]) {
        return;  // there already
    }
    if (index == s_desk.travelling) {
        const bool pressed = s_held != index;  // a held one never was, so has nothing to stop
        arrived();  // the tap that stops it
        if (pressed) {
            press(index);
        }
        return;
    }
    // On its way elsewhere: stopped first, as the box takes the next key as a stop anyway.
    const bool was_going = s_desk.travelling >= 0 && s_held != s_desk.travelling;
    if (was_going) {
        press(s_desk.travelling);
    }
    forget_held();
    s_desk.travelling = index;
    s_moved_at        = lv_tick_get();
    if (s_still_timer == nullptr) {
        s_still_timer = lv_timer_create(check_still, STILL_CHECK_MS, nullptr);
    }
    publish(Topic::Desk);
    if (was_going || coasting()) {
        s_held       = index;
        s_height_at  = lv_tick_get();  // the stop just pressed sets it coasting, if it was not yet
        s_held_timer = lv_timer_create(press_held, SETTLE_CHECK_MS, nullptr);
        return;
    }
    press(index);
}

void lights_take(const char *label, const char *state, bool on)
{
    copy(s_lights.label, sizeof(s_lights.label), label);
    copy(s_lights.state, sizeof(s_lights.state), state);
    s_lights.on = on;
    publish(Topic::Lights);
}

void lights_take_light(int index, const char *name, const char *state, bool on)
{
    LightState &light = s_lights.lights[index];
    copy(light.name, sizeof(light.name), name);
    copy(light.state, sizeof(light.state), state);
    s_lights.light_on[index] = light.name[0] != '\0' && on;
    publish(Topic::Lights);
}

}  // namespace ui::detail
