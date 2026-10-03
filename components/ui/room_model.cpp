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
constexpr std::uint32_t STOPPED_MS      = 3000;  // a stop from here, which a start soon after waits on
// The box takes no new start for a while after it stops, though the height it
// says, in whole centimetres above a metre, may already stand still: measured
// on the desk, a start 0.75 s after a stop was taken as another stop, one 1.2 s
// after started it.
constexpr std::uint32_t STOP_REST_MS    = 1200;
// Pressed, the desk starts in about half a second; still for longer than this
// it has stopped of its own, and a tap starts it again rather than stopping it.
constexpr std::uint32_t HALTED_MS       = 1500;

DeskState   s_desk;
LightsState s_lights;
std::uint32_t s_moved_at   = 0;  // when the height last changed, or the travel began
std::uint32_t s_height_at  = 0;  // when the height last changed
lv_timer_t   *s_still_timer = nullptr;
int           s_held        = -1;  // the preset waiting for the desk to stand still
std::uint32_t s_stopped_at  = 0;   // when a tap here last stopped the desk
lv_timer_t   *s_held_timer  = nullptr;
lv_timer_t   *s_motion_timer = nullptr;  // to see the height stand still again

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
    if (lv_tick_elaps(s_height_at) < SETTLE_MS || lv_tick_elaps(s_stopped_at) < STOP_REST_MS) {
        return;
    }
    const int index = s_held;
    forget_held();
    ESP_LOGI(TAG, "desk: still, preset %d pressed", index + 1);
    s_moved_at = lv_tick_get();  // its travel starts now
    press(index);
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

namespace {
// Still once the height has not changed for as long as a start takes.
void motion_check(lv_timer_t *)
{
    if (lv_tick_elaps(s_height_at) < HALTED_MS) {
        return;
    }
    lv_timer_delete(s_motion_timer);
    s_motion_timer = nullptr;
    s_desk.moving  = 0;
    publish(Topic::Desk);
}
}  // namespace

void desk_take_height(int height_mm)
{
    if (height_mm != s_desk.height_mm) {
        s_moved_at  = lv_tick_get();
        s_height_at = s_moved_at;
        if (s_desk.height_mm >= 0 && height_mm >= 0) {
            s_desk.moving = height_mm > s_desk.height_mm ? 1 : -1;
            if (s_motion_timer == nullptr) {
                s_motion_timer = lv_timer_create(motion_check, HALTED_MS / 3, nullptr);
            }
        }
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

// As the box's own keys: any preset tapped while the desk is on its way stops
// it. But a start tapped just after a stop from here would reach the box while
// the desk still coasts, and only stop it again, so that one waits until the
// desk has stood still.
void desk_go_to(int index)
{
    if (index < 0 || index >= kPresetCount) {
        return;
    }
    if (s_stopped_at != 0 && lv_tick_elaps(s_stopped_at) >= STOPPED_MS) {
        s_stopped_at = 0;
    }
    if (s_desk.travelling >= 0 && s_held >= 0) {
        // Not pressed yet, so nothing under way: tapped again it is dropped, another one waits instead.
        ESP_LOGI(TAG, "desk: preset %d tapped while %d waits", index + 1, s_held + 1);
        if (index == s_held) {
            arrived();
        } else {
            s_held = s_desk.travelling = index;
            publish(Topic::Desk);
        }
        return;
    }
    if (s_desk.travelling >= 0 && lv_tick_elaps(s_moved_at) >= HALTED_MS) {
        arrived();  // it stopped of its own, at the desk or short of the preset
    }
    if (s_desk.travelling >= 0) {
        ESP_LOGI(TAG, "desk: preset %d tapped on the way to %d: stop", index + 1, s_desk.travelling + 1);
        arrived();
        press(index);
        s_stopped_at = lv_tick_get();
        return;
    }
    if (s_desk.preset_active[index]) {
        return;  // there already
    }
    s_desk.travelling = index;
    s_moved_at        = lv_tick_get();
    if (s_still_timer == nullptr) {
        s_still_timer = lv_timer_create(check_still, STILL_CHECK_MS, nullptr);
    }
    publish(Topic::Desk);
    const bool resting = s_stopped_at != 0 && lv_tick_elaps(s_stopped_at) < STOP_REST_MS;
    if (resting || (s_stopped_at != 0 && coasting())) {
        ESP_LOGI(TAG, "desk: preset %d tapped while it comes to a stop: once still", index + 1);
        s_held       = index;
        s_held_timer = lv_timer_create(press_held, SETTLE_CHECK_MS, nullptr);
        return;
    }
    ESP_LOGI(TAG, "desk: preset %d tapped: go", index + 1);
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

void ui::desk_tap(int index)
{
    detail::desk_go_to(index);
}
