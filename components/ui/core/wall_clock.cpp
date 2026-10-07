#include "ui_core.h"

#include <cstdio>

namespace ui::detail {
namespace {
const char *s_pinned_clock = nullptr;
std::time_t s_clock_shift  = 0;  // from the real time to the pinned clock's
}  // namespace

const char *pinned_clock() { return s_pinned_clock; }

std::time_t wall_now() { return std::time(nullptr) + s_clock_shift; }

}  // namespace ui::detail

namespace ui {
void pin_clock(const char *text)
{
    using namespace detail;
    s_pinned_clock = text;
    s_clock_shift  = 0;
    int hour = 0, minute = 0;
    if (text == nullptr || std::sscanf(text, "%d:%d", &hour, &minute) != 2) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm           local{};
    localtime_r(&now, &local);
    local.tm_hour = hour;
    local.tm_min  = minute;
    local.tm_sec  = 0;
    s_clock_shift = std::mktime(&local) - now;
}
}  // namespace ui
