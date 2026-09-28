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

constexpr PillSpec PILLS[] = {
    {"sensor.office_awair_carbon_dioxide", "CO2", 0.0f, 800.0f, 0.0f, 1200.0f},
    {"sensor.office_awair_volatile_organic_compounds_parts", "VOC", 0.0f, 333.0f, 0.0f,
     1000.0f},
    {"sensor.office_awair_humidity", "HUMIDITY", 40.0f, 60.0f, 30.0f, 70.0f},
    {"sensor.office_awair_pm2_5", "PM2.5", 0.0f, 12.0f, 0.0f, 35.0f},
};

struct LightSpec {
    const char *entity;
    const char *name;
};

constexpr LightSpec LIGHTS[] = {
    {"light.office_bureaulamp", "Desk lamp"},
    {"light.office_lamp_muur", "Wall lamp"},
    {"light.office_bed", "Bed"},
    {"light.office_grote_lamp", "Main lamp"},
};

constexpr char ALL_LIGHTS_ENTITY[] = "input_boolean.office_verlichting_actief";
constexpr char ALL_LIGHTS_ON[]     = "script.office_verlichting_aan";
constexpr char ALL_LIGHTS_OFF[]    = "script.office_verlichting_uit";

struct ToggleSpec {
    const char *entity;
    const char *on_label;
    const char *off_label;
};

constexpr ToggleSpec TOGGLES[] = {
    {"input_boolean.office_alleen_kast", "1", "2"},
};

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
