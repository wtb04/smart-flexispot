#pragma once

#include "esp_err.h"

namespace power {

struct State {
    bool  present;       // false when no pack is fitted
    float bus_volts;     // battery / system rail
    float current_amps;  // sign depends on which way the shunt is wired
    int   percent;       // approximate, and meaningless unless present
    float shunt_millivolts;  // raw drop across the shunt, for diagnosis
    bool  charging;          // current flowing into the pack
    bool  on_battery;        // pack supplying the panel, so no external power
};

/** Sets up the INA226 on the board's I2C bus. Requires bsp_i2c_init() first. */
esp_err_t init();

/**
 * @brief Turns the battery charger on or off.
 *
 * The Tab5 boots with charging disabled -- it can only charge once the device
 * is powered on and software enables it -- so without this the pack never
 * charges and the current reading sits at zero whatever is plugged in.
 */
esp_err_t set_charging(bool enable);


/**
 * @brief Puts CHG_EN back if something cleared it.
 *
 * Re-initialising the IO expander resets it to power-on defaults, and the
 * BSP does that when enabling Wi-Fi. Cheap enough to call on every poll.
 */
void reassert_charging();

/** Takes a reading. Safe from any task once init() has returned. */
esp_err_t read(State &out);

}  // namespace power
