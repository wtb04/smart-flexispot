#include "power.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/FreeRTOS.h"
#include "units.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>

namespace power {
namespace {
constexpr char TAG[] = "power";

constexpr std::uint8_t  INA226_ADDRESS = 0x41;
constexpr std::uint32_t I2C_SPEED_HZ   = 400000;
constexpr int           I2C_TIMEOUT_MS = 100;

constexpr std::uint8_t REG_CONFIG        = 0x00;
constexpr std::uint8_t REG_SHUNT_VOLTAGE = 0x01;
constexpr std::uint8_t REG_BUS_VOLTAGE   = 0x02;
constexpr std::uint8_t REG_CURRENT       = 0x04;
constexpr std::uint8_t REG_CALIBRATION   = 0x05;
constexpr std::uint8_t REG_MANUFACTURER  = 0xfe;
constexpr std::uint8_t REG_DIE_ID        = 0xff;

constexpr std::uint16_t MANUFACTURER_TI = 0x5449;  // "TI"
constexpr std::uint16_t DIE_INA226      = 0x2260;

// Configuration fields: sixteen samples averaged, 1.1 ms conversions of both
// the bus and the shunt, both measured continuously.
constexpr int           CONFIG_AVERAGING_SHIFT        = 9;
constexpr int           CONFIG_BUS_TIME_SHIFT         = 6;
constexpr int           CONFIG_SHUNT_TIME_SHIFT       = 3;
constexpr std::uint16_t AVERAGE_16_SAMPLES            = 0b010;
constexpr std::uint16_t CONVERSION_1100_US            = 0b100;
constexpr std::uint16_t MODE_SHUNT_AND_BUS_CONTINUOUS = 0b111;
constexpr std::uint16_t CONFIG_VALUE = (AVERAGE_16_SAMPLES << CONFIG_AVERAGING_SHIFT) |
                                       (CONVERSION_1100_US << CONFIG_BUS_TIME_SHIFT) |
                                       (CONVERSION_1100_US << CONFIG_SHUNT_TIME_SHIFT) |
                                       MODE_SHUNT_AND_BUS_CONTINUOUS;

// The config write restarts conversion and sixteen averaged samples take
// about 35 ms; reading straight away returns zero volts, which reads as
// "no pack".
constexpr TickType_t FIRST_CONVERSION = pdMS_TO_TICKS(60);

constexpr float SHUNT_OHMS        = 0.005f;
// As M5 has it: the pack takes about 0.4 A charging and more discharging under
// load, and at 2 A full scale the register overflows into nonsense.
constexpr float MAX_CURRENT_AMPS  = 8.192f;
constexpr float CURRENT_STEPS     = 32768.0f;  // the positive half of the signed register
constexpr float CURRENT_LSB       = MAX_CURRENT_AMPS / CURRENT_STEPS;
constexpr float CALIBRATION_SCALE = 0.00512f;  // the datasheet's constant
constexpr float BUS_VOLTAGE_LSB   = 0.00125f;
constexpr float SHUNT_LSB_MV      = 0.0025f;

struct CellPoint {
    float volts;
    int   percent;
};
constexpr std::array<CellPoint, 11> CELL_CURVE{{
    {4.115f, 100}, {4.05f, 90}, {3.98f, 80}, {3.89f, 70}, {3.79f, 60}, {3.70f, 45},
    {3.60f, 30},   {3.50f, 15}, {3.40f, 7},  {3.20f, 2},  {3.00f, 0},
}};
constexpr int CELLS_IN_SERIES = 2;

constexpr float      PACK_MIN_VOLTS = 6.0f;
constexpr float      PACK_MAX_VOLTS = 8.8f;
constexpr TickType_t PROBE_SETTLE   = pdMS_TO_TICKS(250);

// The gap between these two is the hysteresis: without it the pack relaxes a
// few millivolts, asks for more, and is topped up for ever.
constexpr float FULL_VOLTS   = 8.30f;
constexpr float FULL_TAPER_A = 0.06f;

constexpr float PACK_RESISTANCE_OHMS = 0.24f;

constexpr float PERCENT_TAU_S = 30.0f;

constexpr float CURRENT_DEADBAND_A = 0.01f;

constexpr esp_io_expander_pin_num_t CHARGE_ENABLE_PIN = IO_EXPANDER_PIN_NUM_7;
// Active low, and nothing to do with USB quick-charge despite the name: it
// gates a resistor across the charger's NTC pin, which halves the current.
constexpr esp_io_expander_pin_num_t CHARGE_QC_PIN = IO_EXPANDER_PIN_NUM_5;

i2c_master_dev_handle_t s_dev = nullptr;
bool                    s_charging_wanted = false;

portMUX_TYPE s_last_lock = portMUX_INITIALIZER_UNLOCKED;
State        s_last{};
bool         s_have_last = false;

SemaphoreHandle_t s_read_lock = nullptr;
StaticSemaphore_t s_read_lock_ctrl;

float        s_percent  = 0.0f;
bool         s_smoothed = false;
std::int64_t s_percent_us = 0;

std::atomic<bool> s_pack_present{false};

esp_err_t read_register(std::uint8_t reg, std::uint16_t &out)
{
    std::array<std::uint8_t, sizeof(out)> rx{};
    ESP_RETURN_ON_ERROR(
        i2c_master_transmit_receive(s_dev, &reg, 1, rx.data(), rx.size(), I2C_TIMEOUT_MS), TAG,
        "read reg 0x%02x", reg);
    out = static_cast<std::uint16_t>((rx[0] << CHAR_BIT) | rx[1]);
    return ESP_OK;
}

esp_err_t write_register(std::uint8_t reg, std::uint16_t value)
{
    const std::array<std::uint8_t, 1 + sizeof(value)> tx{
        reg, static_cast<std::uint8_t>(value >> CHAR_BIT), static_cast<std::uint8_t>(value)};
    return i2c_master_transmit(s_dev, tx.data(), tx.size(), I2C_TIMEOUT_MS);
}

bool pack_voltage(float volts)
{
    return volts >= PACK_MIN_VOLTS && volts <= PACK_MAX_VOLTS;
}

int percent_for(float pack_volts)
{
    const float cell = pack_volts / CELLS_IN_SERIES;
    if (cell >= CELL_CURVE.front().volts) {
        return CELL_CURVE.front().percent;
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
    return CELL_CURVE.back().percent;
}

struct Raw {
    std::uint16_t bus     = 0;
    std::uint16_t current = 0;
    std::uint16_t shunt   = 0;
};

esp_err_t read_raw(Raw &raw)
{
    ESP_RETURN_ON_ERROR(read_register(REG_BUS_VOLTAGE, raw.bus), TAG, "bus voltage");
    ESP_RETURN_ON_ERROR(read_register(REG_CURRENT, raw.current), TAG, "current");
    ESP_RETURN_ON_ERROR(read_register(REG_SHUNT_VOLTAGE, raw.shunt), TAG, "shunt voltage");
    return ESP_OK;
}

int smoothed_percent(bool present, int measured)
{
    const std::int64_t now = esp_timer_get_time();
    if (!present) {
        s_smoothed = false;
    } else if (!s_smoothed) {
        s_percent  = static_cast<float>(measured);
        s_smoothed = true;
    } else {
        const float elapsed =
            static_cast<float>(now - s_percent_us) / static_cast<float>(units::kUsPerSecond);
        s_percent += (static_cast<float>(measured) - s_percent) *
                     (1.0f - std::exp(-elapsed / PERCENT_TAU_S));
    }
    s_percent_us = now;
    return present ? static_cast<int>(std::lround(s_percent)) : 0;
}

void remember(const State &state)
{
    portENTER_CRITICAL(&s_last_lock);
    s_last      = state;
    s_have_last = true;
    portEXIT_CRITICAL(&s_last_lock);
}

esp_err_t read_locked(State &out)
{
    Raw raw;
    ESP_RETURN_ON_ERROR(read_raw(raw), TAG, "registers");
    out.shunt_millivolts = static_cast<std::int16_t>(raw.shunt) * SHUNT_LSB_MV;

    out.bus_volts    = static_cast<std::int16_t>(raw.bus) * BUS_VOLTAGE_LSB;
    out.current_amps = static_cast<std::int16_t>(raw.current) * CURRENT_LSB;
    if (!pack_voltage(out.bus_volts)) {
        s_pack_present.store(false, std::memory_order_relaxed);
    }
    out.present      = s_pack_present.load(std::memory_order_relaxed);
    out.on_battery   = out.present && out.current_amps > CURRENT_DEADBAND_A;
    out.charging     = out.present && out.current_amps < -CURRENT_DEADBAND_A;
    out.full         = out.present && out.bus_volts >= FULL_VOLTS &&
                       out.current_amps > -FULL_TAPER_A;

    const float open_circuit = out.bus_volts + out.current_amps * PACK_RESISTANCE_OHMS;
    const int   measured     = out.present ? percent_for(open_circuit) : 0;
    out.percent              = smoothed_percent(out.present, measured);

    remember(out);
    return ESP_OK;
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
    s_read_lock = xSemaphoreCreateMutexStatic(&s_read_lock_ctrl);
    ESP_RETURN_ON_FALSE(s_read_lock != nullptr, ESP_ERR_NO_MEM, TAG, "read lock");

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
    const auto calibration =
        static_cast<std::uint16_t>(CALIBRATION_SCALE / (CURRENT_LSB * SHUNT_OHMS));
    ESP_RETURN_ON_ERROR(write_register(REG_CALIBRATION, calibration), TAG, "calibration");
    vTaskDelay(FIRST_CONVERSION);

    State probe{};
    ESP_RETURN_ON_ERROR(read(probe), TAG, "first read");

    bool present = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(probe_pack(present));
    ESP_LOGI(TAG, "%s", present ? "pack present" : "no pack");
    return ESP_OK;
}

namespace {
esp_err_t apply_charging(bool enable)
{
    esp_io_expander_handle_t expander = bsp_io_expander1_init();
    ESP_RETURN_ON_FALSE(expander != nullptr, ESP_ERR_INVALID_STATE, TAG, "io expander");

    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(expander, CHARGE_ENABLE_PIN | CHARGE_QC_PIN,
                                                IO_EXPANDER_OUTPUT),
                        TAG, "charge pin dir");
    // The PI4IOE5V6408 resets with every pin high-impedance, so a direction and
    // a level alone leave the pin floating and the charger off.
    ESP_RETURN_ON_ERROR(esp_io_expander_set_output_mode(expander,
                                                        CHARGE_ENABLE_PIN | CHARGE_QC_PIN,
                                                        IO_EXPANDER_OUTPUT_MODE_PUSH_PULL),
                        TAG, "charge pin drive");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, CHARGE_QC_PIN, enable ? 0 : 1), TAG,
                        "charge current");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(expander, CHARGE_ENABLE_PIN, enable ? 1 : 0),
                        TAG, "charge pin level");
    return ESP_OK;
}

}  // namespace

esp_err_t set_charging(bool enable)
{
    s_charging_wanted = enable;
    ESP_RETURN_ON_ERROR(apply_charging(enable), TAG, "charge pins");
    ESP_LOGI(TAG, "charger %s", enable ? "enabled (fast)" : "disabled");
    return ESP_OK;
}

bool charging_enabled()
{
    return s_charging_wanted;
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
    esp_io_expander_set_output_mode(expander, CHARGE_ENABLE_PIN | CHARGE_QC_PIN,
                                    IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    esp_io_expander_set_level(expander, CHARGE_QC_PIN, 0);
    esp_io_expander_set_level(expander, CHARGE_ENABLE_PIN, 1);
}

esp_err_t read(State &out)
{
    ESP_RETURN_ON_FALSE(s_dev != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_read_lock, portMAX_DELAY);
    const esp_err_t err = read_locked(out);
    xSemaphoreGive(s_read_lock);
    return err;
}

esp_err_t probe_pack(bool &present)
{
    State now{};
    const esp_err_t err = read(now);
    if (err != ESP_OK) {
        present = s_pack_present.load(std::memory_order_relaxed);
        return err;
    }

    if (std::fabs(now.current_amps) > CURRENT_DEADBAND_A || !s_charging_wanted) {
        present = pack_voltage(now.bus_volts);
        s_pack_present.store(present, std::memory_order_relaxed);
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(apply_charging(false), TAG, "charger off for probe");
    vTaskDelay(PROBE_SETTLE);

    State settled{};
    const esp_err_t settled_err = read(settled);
    present = settled_err == ESP_OK && pack_voltage(settled.bus_volts);
    s_pack_present.store(present, std::memory_order_relaxed);

    // Back as it was asked to be, pack or not: whether there is one worth
    // charging is the charger's call, and it cannot make it switched off.
    ESP_ERROR_CHECK_WITHOUT_ABORT(apply_charging(s_charging_wanted));
    return settled_err;
}

bool last(State &out)
{
    portENTER_CRITICAL(&s_last_lock);
    const bool have = s_have_last;
    out             = s_last;
    portEXIT_CRITICAL(&s_last_lock);
    return have;
}

}  // namespace power
