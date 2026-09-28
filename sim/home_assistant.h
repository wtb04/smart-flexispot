#pragma once

// Home Assistant, played: the simulator never talks to the real one. Until it
// "answers" the home page shows what the panel shows without it; once it does,
// the readings, lights and thermostat have states, and the page's taps change them.
namespace home_assistant {
void start();

/** Answering, with states; or gone, which leaves the page as before it answered. */
void toggle();

/** Readings good, then some not, then bad. */
void next_air();

void on_lights();
void on_light(int index);
void on_setpoint(float celsius);
void on_mode();
void on_dial_toggle(int index);
}  // namespace home_assistant
