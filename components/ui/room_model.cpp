#include "room_model.h"

#include "topics.h"
#include "ui_internal.h"

#include <cstdio>

namespace ui::detail {
namespace {
// A travel that has not moved the desk for this long has ended short of its
// preset, stopped at the desk or by another button: no longer shown as going.
constexpr std::uint32_t STILL_MS       = 4000;
constexpr std::uint32_t STILL_CHECK_MS = 500;

DeskState   s_desk;
LightsState s_lights;
std::uint32_t s_moved_at   = 0;  // when the height last changed, or the travel began
lv_timer_t   *s_still_timer = nullptr;

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

void arrived()
{
    stop_watching();
    s_desk.travelling = -1;
    publish(Topic::Desk);
}

void check_still(lv_timer_t *)
{
    if (lv_tick_elaps(s_moved_at) >= STILL_MS) {
        arrived();
    }
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
        s_moved_at = lv_tick_get();
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
        arrived();  // the tap that stops it
    } else {
        s_desk.travelling = index;
        s_moved_at        = lv_tick_get();
        if (s_still_timer == nullptr) {
            s_still_timer = lv_timer_create(check_still, STILL_CHECK_MS, nullptr);
        }
        publish(Topic::Desk);
    }
    if (s_handlers.preset != nullptr) {
        s_handlers.preset(index, false);
    }
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
