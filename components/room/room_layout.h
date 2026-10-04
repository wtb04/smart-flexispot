#pragma once

#include "ui.h"

// What the home page holds, and how it looks before Home Assistant has said
// anything. Apart from the talk with Home Assistant, so the desktop
// simulator shows the same.
namespace room {
struct PillSpec {
    const char *entity;
    const char *label;
    float       good_lo, good_hi;
    float       warn_lo, warn_hi;
};

struct LightSpec {
    const char *entity;
    const char *name;
};

struct ToggleSpec {
    const char *entity;
    const char *on_label;
    const char *off_label;
};

// Which entities: room_config.h, which git ignores, or the example beside it.
#if __has_include("room_config.h")
#include "room_config.h"
#else
#include "room_config.example.h"
#endif

inline const char *toggle_label(const ToggleSpec &spec, bool on)
{
    return on ? spec.on_label : spec.off_label;
}

constexpr int PILL_COUNT   = sizeof(PILLS) / sizeof(PILLS[0]);
constexpr int LIGHT_COUNT  = sizeof(LIGHTS) / sizeof(LIGHTS[0]);
constexpr int TOGGLE_COUNT = sizeof(TOGGLES) / sizeof(TOGGLES[0]);

static_assert(PILL_COUNT <= ui::kPillCount, "more readings than the strip has chips");
static_assert(LIGHT_COUNT <= ui::kLightCount, "more lights than the picker has buttons");
static_assert(TOGGLE_COUNT <= ui::kDialToggleCount, "more toggles than the dial has corners");

/** Every reading, light and toggle by name, with nothing known yet. */
void show_unknown();
}  // namespace room
