#include "power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace power {
namespace {

constexpr char TAG[] = "power";

constexpr std::uint8_t  INA226_ADDRESS = 0x41;
constexpr std::uint32_t I2C_SPEED_HZ   = 400000;
constexpr int           I2C_TIMEOUT_MS = 100;

constexpr std::uint8_t REG_CONFIG      = 0x00;
constexpr std::uint8_t REG_BUS_VOLTAGE = 0x02;
constexpr std::uint8_t REG_CURRENT     = 0x04;
constexpr std::uint8_t REG_CALIBRATION = 0x05;
constexpr std::uint8_t REG_SHUNT_VOLTAGE = 0x01;
constexpr std::uint8_t REG_MANUFACTURER  = 0xfe;
constexpr std::uint8_t REG_DIE_ID        = 0xff;

// The audio ADC also answers in this address range, so the address alone is not
// enough to go on.
constexpr std::uint16_t MANUFACTURER_TI = 0x5449;  // "TI"
constexpr std::uint16_t DIE_INA226      = 0x2260;

// 16 averages, 1.1 ms conversions, shunt and bus continuous.
constexpr std::uint16_t CONFIG_VALUE = (0b010 << 9) | (0b100 << 6) | (0b100 << 3) | 0b111;

// 2 A full scale rather than 8.192 A: the panel never draws near 8 A, and the
// lower range gives four times the current resolution, which is what the
// charge/discharge deadband depends on. Matches M5Unified.
constexpr float SHUNT_OHMS       = 0.005f;
constexpr float MAX_CURRENT_AMPS = 2.0f;
constexpr float CURRENT_LSB      = MAX_CURRENT_AMPS / 32768.0f;
constexpr float BUS_VOLTAGE_LSB  = 0.00125f;

struct CellPoint {
    float volts;
    int   percent;
};
// Lithium cells sag in a curve, so interpolating a straight line between empty
// and full reads badly wrong through the middle. Anchored to this pack: M5
// document it full at 8.23 V and shutting down at 6.0 V, so 4.115 and 3.0 per
// cell are 100% and 0%. Approximate, and per cell -- the pack is two in series.
constexpr std::array<CellPoint, 11> CELL_CURVE{{
    {4.115f, 100}, {4.05f, 90}, {3.98f, 80}, {3.89f, 70}, {3.79f, 60}, {3.70f, 45},
    {3.60f, 30},   {3.50f, 15}, {3.40f, 7},  {3.20f, 2},  {3.00f, 0},
}};
constexpr int CELLS_IN_SERIES = 2;

// An empty socket floats around 1.9 V and a flat pack disconnects near 2.75 V
// per cell, so anything under this is no pack rather than a dead one.
constexpr float PACK_PRESENT_VOLTS = 4.0f;

// CHG_EN sits on bit 7 of the second IO expander (0x44), left clear by M5's own
// bring-up.
constexpr esp_io_expander_pin_num_t CHARGE_ENABLE_PIN = IO_EXPANDER_PIN_NUM_7;

// The shunt reads positive on discharge and negative on charge. At 2 A full
// scale the noise floor is far below this deadband.
constexpr float CURRENT_DEADBAND_A = 0.01f;

// The charger must not be switched on into a collapsed or absent pack: a Tab5
// pack below 6 V has to be removed and refitted before it will charge.
constexpr float CHARGE_SAFE_VOLTS = 6.0f;

// Active low, and nothing to do with USB quick-charge despite the name: it
// gates a resistor across the charger's NTC pin. Released, the IP2326 charges at
// its full ~1 A; asserted, the pin reads "warm" and the chip halves the current.
constexpr esp_io_expander_pin_num_t CHARGE_QC_PIN = IO_EXPANDER_PIN_NUM_5;

i2c_master_dev_handle_t s_dev = nullptr;
bool                    s_charging_wanted = false;

esp_err_t read_register(std::uint8_t reg, std::uint16_t &out)
{
    std::array<std::uint8_t, 2> rx{};
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(s_dev, &reg, 1, rx.data(), rx.size(), I2C_TIMEOUT_MS), TAG,
        "read reg 0x%02x", reg);
    out = static_cast<std::uint16_t>((rx[0] << 8) | rx[1]);
    return ESP_OK;
}

esp_err_t write_register(std::uint8_t reg, std::uint16_t value)
{
    const std::array<std::uint8_t, 3> tx{reg, static_cast<std::uint8_t>(value >> 8),
                                         static_cast<std::uint8_t>(value & 0xff)};
    return i2c_master_transmit(s_dev, tx.data(), tx.size(), I2C_TIMEOUT_MS);
}

int percent_for(float pack_volts)
{
    const float cell = pack_volts / CELLS_IN_SERIES;
    if (cell >= CELL_CURVE.front().volts) {
        return 100;
    }
    for (std::size_t i = 1; i < CELL_CURVE.size(); ++i) {
        const CellPoint &hi = CELL_CURVE[i - 1];
        const CellPoint &lo = CELL_CURVE[i];
        if (cell >= lo.volts) {
            const float span = hi.volts - lo.volts;
            const float frac = span > 0.0f ? (cell - lo.volts) / span : 0.0f;
            return lo.percent + static_cast<int>(std::lround(frac * (hi.percent - lo.percent)));
        }
    }
    return 0;
}

}  // namespace

esp_err_t init()
{
    ESP_RETURN_ON_FALSE(s_dev == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_STATE, TAG, "i2c not initialised");

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = INA226_ADDRESS,
        .scl_speed_hz    = I2C_SPEED_HZ,
        .scl_wait_us     = 0,
        .flags           = {},
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    std::uint16_t manufacturer = 0;
    std::uint16_t die = 0;
    ESP_RETURN_ON_ERROR(read_register(REG_MANUFACTURER, manufacturer), TAG, "manufacturer id");
    ESP_RETURN_ON_ERROR(read_register(REG_DIE_ID, die), TAG, "die id");
    ESP_LOGI(TAG, "id at 0x%02x: manufacturer 0x%04x die 0x%04x", INA226_ADDRESS, manufacturer,
             die);
    ESP_RETURN_ON_FALSE(manufacturer == MANUFACTURER_TI && die == DIE_INA226, ESP_ERR_NOT_FOUND,
                        TAG, "not an INA226 at 0x%02x", INA226_ADDRESS);

    ESP_RETURN_ON_ERROR(write_register(REG_CONFIG, CONFIG_VALUE), TAG, "config");
    // Datasheet calibration: 0.00512 / (current LSB * shunt).
    const auto calibration =
        static_cast<std::uint16_t>(0.00512f / (CURRENT_LSB * SHUNT_OHMS));
    ESP_RETURN_ON_ERROR(write_register(REG_CALIBRATION, calibration), TAG, "calibration");

    // The config write restarts conversion, and sixteen averaged samples take
    // about 35 ms. Reading straight away returns zero volts, which reads as "no
    // pack" and leaves the charger off for the whole uptime.
    vTaskDelay(pdMS_TO_TICKS(60));

    State probe{};
    ESP_RETURN_ON_ERROR(read(probe), TAG, "first read");
    if (probe.bus_volts >= CHARGE_SAFE_VOLTS) {
        ESP_RETURN_ON_ERROR(set_charging(true), TAG, "charger");
    } else {
        ESP_LOGW(TAG, "pack at %.2f V, below the %.1f V floor - charger left off",
                 probe.bus_volts, CHARGE_SAFE_VOLTS);
    }
    return ESP_OK;
}

esp_err_t set_charging(bool enable)
{
    esp_io_expander_handle_t expander = bsp_io_expander1_init();
    ESP_RETURN_ON_FALSE(expander != nullptr, ESP_ERR_INVALID_STATE, TAG, "io expander");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, CHARGE_ENABLE_PIN | CHARGE_QC_PIN,
                                                IO_EXPANDER_OUTPUT),
                        TAG, "charge pin dir");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, CHARGE_QC_PIN, enable ? 0 : 1), TAG,
                        "charge current");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, CHARGE_ENABLE_PIN, enable ? 1 : 0),
                        TAG, "charge pin level");
    s_charging_wanted = enable;
    ESP_LOGI(TAG, "charger %s", enable ? "enabled (fast)" : "disabled");
    return ESP_OK;
}

// Re-initialising the 0x44 expander resets it to power-on defaults, clearing
// CHG_EN, and the BSP does that when it enables Wi-Fi. Written unconditionally:
// esp_io_expander_get_level() returns the input register, which reads low for a
// pin driven as an output, so a read-back cannot tell cleared from set.
void reassert_charging()
{
    if (!s_charging_wanted) {
        return;
    }
    esp_io_expander_handle_t expander = bsp_io_expander1_init();
    if (expander == nullptr) {
        return;
    }
    esp_io_expander_set_dir(expander, CHARGE_ENABLE_PIN | CHARGE_QC_PIN, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(expander, CHARGE_QC_PIN, 0);
    esp_io_expander_set_level(expander, CHARGE_ENABLE_PIN, 1);
}

esp_err_t read(State &out)
{
    ESP_RETURN_ON_FALSE(s_dev != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");

    std::uint16_t raw_bus = 0;
    std::uint16_t raw_current = 0;
    std::uint16_t raw_shunt = 0;
    ESP_RETURN_ON_ERROR(read_register(REG_BUS_VOLTAGE, raw_bus), TAG, "bus voltage");
    ESP_RETURN_ON_ERROR(read_register(REG_CURRENT, raw_current), TAG, "current");
    ESP_RETURN_ON_ERROR(read_register(REG_SHUNT_VOLTAGE, raw_shunt), TAG, "shunt voltage");
    out.shunt_millivolts = static_cast<std::int16_t>(raw_shunt) * 0.0025f;

    out.bus_volts    = static_cast<std::int16_t>(raw_bus) * BUS_VOLTAGE_LSB;
    out.current_amps = static_cast<std::int16_t>(raw_current) * CURRENT_LSB;
    out.present      = out.bus_volts >= PACK_PRESENT_VOLTS;
    out.percent      = out.present ? percent_for(out.bus_volts) : 0;
    out.on_battery   = out.present && out.current_amps > CURRENT_DEADBAND_A;
    out.charging     = out.present && out.current_amps < -CURRENT_DEADBAND_A;
    return ESP_OK;
}

}  // namespace power
