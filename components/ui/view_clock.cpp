#include "ui_internal.h"

#include <cstring>
#include <ctime>
#include <vector>

// The time beside a fullscreen view's way back, there while its buttons are.
namespace ui::detail {
namespace {
constexpr std::int32_t  CHIP_GAP  = 20;
constexpr std::uint32_t TICK_MS   = 1000;
constexpr time_t        CLOCK_SET = 1'700'000'000;  // any earlier and the clock is not set yet

struct Clock {
    lv_obj_t *label;
    lv_obj_t *chip;
    lv_area_t beside;  // the chip it was put beside, where it was then
};
std::vector<Clock> s_clocks;

void tell_time(lv_timer_t *)
{
    update_view_clocks();
}
}  // namespace

void update_view_clocks()
{
    const time_t now = std::time(nullptr);
    std::tm      local{};
    localtime_r(&now, &local);
    char text[8] = "";
    if (now >= CLOCK_SET) {
        std::strftime(text, sizeof(text), "%H:%M", &local);
    }
    for (Clock &clock : s_clocks) {
        const bool changed = std::strcmp(lv_label_get_text(clock.label), text) != 0;
        if (changed) {
            theme::set_text(clock.label, text);
        }
        lv_area_t chip{};
        lv_obj_update_layout(clock.chip);
        lv_obj_get_coords(clock.chip, &chip);
        if (changed || std::memcmp(&chip, &clock.beside, sizeof(chip)) != 0) {
            clock.beside = chip;
            lv_obj_align_to(clock.label, clock.chip, LV_ALIGN_OUT_LEFT_MID, -CHIP_GAP, 0);
        }
    }
}

lv_obj_t *add_view_clock(ViewId view, lv_obj_t *root, lv_obj_t *chip)
{
    lv_obj_t *label = theme::make_label(root, "", theme::text, fonts::size_28());
    lv_obj_set_clickable(label, false);
    if (s_clocks.empty()) {
        lv_timer_create(tell_time, TICK_MS, nullptr);
    }
    s_clocks.push_back({label, chip, {}});
    fade_when_idle(view, label);
    update_view_clocks();
    return label;
}
}  // namespace ui::detail
