#pragma once

// The Home Assistant entities the panel shows and works. Copy this file to
// room_config.h, which git ignores, and put your own in; without one, the panel
// is built with these. Included by room_layout.h, in room::.

// The readings along the top, with the ranges that colour them: green inside
// good, amber inside warn, red outside both.
constexpr PillSpec PILLS[] = {
    {"sensor.office_carbon_dioxide", "CO2", 0.0f, 800.0f, 0.0f, 1200.0f},
    {"sensor.office_volatile_organic_compounds_parts", "VOC", 0.0f, 333.0f, 0.0f, 1000.0f},
    {"sensor.office_humidity", "HUMIDITY", 40.0f, 60.0f, 30.0f, 70.0f},
    {"sensor.office_pm2_5", "PM2.5", 0.0f, 12.0f, 0.0f, 35.0f},
};

// The lights, each a button when the lights are held.
constexpr LightSpec LIGHTS[] = {
    {"light.office_desk_lamp", "Desk lamp"},
    {"light.office_wall_lamp", "Wall lamp"},
    {"light.office_floor_lamp", "Floor lamp"},
    {"light.office_ceiling", "Main lamp"},
};

// All the lights at once: whether any is on, and a script to turn them on and
// one to turn them off.
constexpr char ALL_LIGHTS_ENTITY[] = "input_boolean.office_lights_on";
constexpr char ALL_LIGHTS_ON[]     = "script.office_lights_on";
constexpr char ALL_LIGHTS_OFF[]    = "script.office_lights_off";

// One of the lights above: with it or the lights all on, someone is in the
// room and the screen stays lit.
constexpr char MAIN_LIGHT_ENTITY[] = "light.office_ceiling";

// Switches in the corners of the heating dial, with what each side is called.
constexpr ToggleSpec TOGGLES[] = {
    {"input_boolean.office_heating_zone", "1", "2"},
};

// The thermostat, and the speaker the music card plays on.
constexpr char CLIMATE_ENTITY[] = "climate.office";
constexpr char MEDIA_SPEAKER[]  = "media_player.office_speaker";

// The favourites, in the order the music card's popup shows them. For another:
// open it in the Spotify app, Share, Copy link; open.spotify.com/playlist/ID is
// spotify:playlist:ID here. A Daily Mix keeps its link as its songs change.
constexpr const char *PICK_URIS[] = {
    "spotify:playlist:37i9dQZEVXbMDoHDwVN2tF",  // Top 50 - Global
    "spotify:playlist:37i9dQZEVXbKCF6dqVpDkS",  // Top 50 - Netherlands
};
