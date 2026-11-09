// What Home's widget says of the weather: the sky's glyph, a temperature, the
// next two hours of rain at home and KNMI's warning.

#include "kit_front.h"

#include "kit_icons.h"
#include "weather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace ui::kitfront {
namespace {
namespace ic = kit::glyph;

constexpr float WET_MM_H      = 0.1f;  // Buienradar's least, 77
constexpr float LIGHT_MM_H    = 1.0f;
constexpr float HEAVY_MM_H    = 5.0f;
constexpr int   MINUTES_A_DAY = 24 * 60;
constexpr int   STEP_MIN      = 5;

// Minutes from now to a step given in minutes past local midnight, either
// side of midnight.
int minutes_from(int now_minute, int minute)
{
    int d = minute - now_minute;
    if (d < -MINUTES_A_DAY / 2) {
        d += MINUTES_A_DAY;
    } else if (d > MINUTES_A_DAY / 2) {
        d -= MINUTES_A_DAY;
    }
    return d;
}

int minute_of(std::int64_t at)
{
    std::tm    when{};
    const auto stamp = static_cast<std::time_t>(at);
    localtime_r(&stamp, &when);
    return when.tm_hour * 60 + when.tm_min;
}

// From the step that holds now on.
int first_current(const weather::RainStep *steps, int count, std::int64_t now)
{
    const int at = minute_of(now);
    for (int i = 0; i < count; ++i) {
        if (minutes_from(at, steps[i].minute) > -STEP_MIN) {
            return i;
        }
    }
    return count;
}

const char *strength(float peak)
{
    return peak < LIGHT_MM_H ? "Light" : peak < HEAVY_MM_H ? "Steady" : "Heavy";
}

// KNMI's own three, a little softer on the panel's dark.
std::uint32_t code_colour(weather::Code code)
{
    switch (code) {
        case weather::Code::Red: return 0xeb5757;
        case weather::Code::Orange: return 0xf2994a;
        default: return 0xf2c94c;
    }
}

const char *code_name(weather::Code code)
{
    return code == weather::Code::Red ? "red" : code == weather::Code::Orange ? "orange" : "yellow";
}

constexpr lv_opa_t PILL_TINT = 40;
constexpr std::int32_t PILL_H = 34;
}  // namespace

const char *sky_glyph(int code, bool day)
{
    switch (weather::sky_of(code)) {
        case weather::Sky::Clear: return day ? ic::sunny : ic::bedtime;
        case weather::Sky::Partly: return day ? ic::partly_cloudy_day : ic::partly_cloudy_night;
        case weather::Sky::Fog: return ic::foggy;
        case weather::Sky::Drizzle:
        case weather::Sky::Rain:
        case weather::Sky::Showers: return ic::rainy;
        case weather::Sky::Snow: return ic::weather_snowy;
        case weather::Sky::Thunder: return ic::thunderstorm;
        default: return ic::cloud;
    }
}

void degrees_text(float celsius, char *out, std::size_t size)
{
    const long whole = std::lround(celsius);
    // a true minus, which the readout fonts carry, rather than a hyphen
    std::snprintf(out, size, whole < 0 ? "\xe2\x88\x92%ld\xc2\xb0" : "%ld\xc2\xb0", whole < 0 ? -whole : whole);
}

bool rain_words(const weather::RainStep *steps, int count, std::int64_t now, char *out, std::size_t size)
{
    const int from = first_current(steps, count, now);
    if (from >= count) {
        return false;
    }
    int first_wet = -1;
    for (int i = from; i < count && first_wet < 0; ++i) {
        first_wet = steps[i].mm_h >= WET_MM_H ? i : -1;
    }
    if (first_wet < 0) {
        std::snprintf(out, size, "Dry for 2 hours");
        return true;
    }
    int   dry  = first_wet;
    float peak = 0.0f;
    while (dry < count && steps[dry].mm_h >= WET_MM_H) {
        peak = std::max(peak, steps[dry].mm_h);
        ++dry;
    }
    const auto clock = [](int minute, char *text, std::size_t n) {
        std::snprintf(text, n, "%02d:%02d", minute / 60 % 24, minute % 60);
    };
    char start[8], end[8];
    clock(steps[first_wet].minute, start, sizeof(start));
    if (dry < count) {
        clock(steps[dry].minute, end, sizeof(end));
    }
    if (first_wet == from) {
        if (dry < count) {
            std::snprintf(out, size, "%s until %s", strength(peak), end);
        } else {
            std::snprintf(out, size, "%s for 2 hours", strength(peak));
        }
    } else if (dry < count) {
        std::snprintf(out, size, "%s, %s to %s", strength(peak), start, end);
    } else {
        std::snprintf(out, size, "%s from %s", strength(peak), start);
    }
    return true;
}

lv_obj_t *warning_pill(lv_obj_t *parent)
{
    lv_obj_t *pill = kit::flow(parent, LV_FLEX_FLOW_ROW, kit::space::s);
    lv_obj_set_height(pill, PILL_H);
    lv_obj_set_style_pad_hor(pill, kit::space::m, 0);
    lv_obj_set_style_radius(pill, kit::radius::round, 0);
    lv_obj_set_style_bg_opa(pill, PILL_TINT, 0);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    kit::icon(pill, ic::warning, 24, true);
    lv_obj_t *words = kit::text(pill, kit::Role::Small, "");
    lv_label_set_long_mode(words, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_hidden(pill, true);
    return pill;
}

void set_warning_pill(lv_obj_t *pill, const weather::Warning *warning, std::int64_t now)
{
    lv_obj_set_hidden(pill, warning == nullptr);
    if (warning == nullptr) {
        return;
    }
    const lv_color_t colour = lv_color_hex(code_colour(warning->code));
    lv_obj_set_style_bg_color(pill, colour, 0);
    lv_obj_set_style_bg_opa(pill, PILL_TINT, 0);
    lv_obj_t *glyph = lv_obj_get_child(pill, 0), *words = lv_obj_get_child(pill, 1);
    lv_obj_set_style_text_color(glyph, colour, 0);
    lv_obj_set_style_text_color(words, colour, 0);
    // from when, while it is still to come; until when, once it holds
    const bool         coming = warning->onset > now;
    const std::int64_t at     = coming ? warning->onset : warning->expires;
    std::tm            when{};
    const auto         stamp = static_cast<std::time_t>(at);
    localtime_r(&stamp, &when);
    char text[64];
    std::snprintf(text, sizeof(text), "Code %s, %s %s %02d:%02d", code_name(warning->code), warning->what,
                  coming ? "from" : "until", when.tm_hour, when.tm_min);
    kit::set_words(words, text);
}

}  // namespace ui::kitfront
