#pragma once

#include "esp_err.h"

namespace power {
struct State {
    bool  present;
    float bus_volts;
    float current_amps;  // positive on discharge, negative on charge
    int   percent;       // approximate, and meaningless unless present
    float shunt_millivolts;
    bool  charging;
    bool  on_battery;
    bool  full;          // at the top of its range and taking nothing more
};

/** Requires bsp_i2c_init() first. */
esp_err_t init();

/** The Tab5 boots with charging disabled, so without this the pack never charges
 *  whatever is plugged in. The IP2326 decides for itself whether there is a pack,
 *  trickles one that is low and stops at full, as M5's own firmware leaves it
 *  to: it switches the charger on at boot and never off. A pack whose protection
 *  has cut it off reads as absent until the charger wakes it, so no reading is
 *  a reason to keep the charger off. */
esp_err_t set_charging(bool enable);

/** Whether the charger is switched on, as opposed to asked for. */
bool charging_enabled();

/** Puts CHG_EN back: re-initialising the IO expander clears it, and the BSP does
 *  that when enabling Wi-Fi. */
void reassert_charging();

/** Safe from any task once init() has returned. */
esp_err_t read(State &out);

/** Settles the question by switching the charger off for a moment and seeing
 *  whether the voltage holds: a pack does, an empty socket does not. Only
 *  actually needed when the charger is on and no current is flowing, so most
 *  calls are an ordinary reading and interrupt nothing; that one case blocks
 *  for a few hundred milliseconds. */
esp_err_t probe_pack(bool &present);

/** The most recent successful read, without touching the bus. False if there
 *  has not been one. */
bool last(State &out);

}  // namespace power
