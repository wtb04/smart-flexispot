#include "backup_clock.h"

#include "clock_math.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include <array>
#include <cstdint>
#include <sys/time.h>

namespace rtc {
namespace {
constexpr char TAG[] = "rtc";

constexpr std::uint8_t  ADDRESS        = 0x32;
constexpr std::uint32_t I2C_SPEED_HZ   = 400000;
constexpr int           I2C_TIMEOUT_MS = 100;

constexpr std::uint8_t REG_SEC   = 0x10;
constexpr std::uint8_t REG_FLAG  = 0x1d;
constexpr std::uint8_t REG_CTRL0 = 0x1e;
constexpr std::uint8_t REG_CTRL1 = 0x1f;

// The time registers from REG_SEC on, in order, each in BCD under a mask.
enum TimeField : std::size_t {
    TIME_SECONDS,
    TIME_MINUTES,
    TIME_HOURS,
    TIME_WEEKDAY,
    TIME_DAY,
    TIME_MONTH,
    TIME_YEAR,
    TIME_FIELDS,
};
constexpr std::uint8_t SECONDS_MASK = 0x7f;
constexpr std::uint8_t MINUTES_MASK = 0x7f;
constexpr std::uint8_t HOURS_MASK   = 0x3f;
constexpr std::uint8_t DAY_MASK     = 0x3f;
constexpr std::uint8_t MONTH_MASK   = 0x1f;

// The chip keeps two digits of year from 2000, and the weekday as one bit of seven.
constexpr int CHIP_YEAR_BASE    = 2000;
constexpr int YEARS_PER_CENTURY = 100;

constexpr std::uint8_t FLAG_VOLTAGE_LOW = 1 << 1;

constexpr std::uint8_t CTRL0_STOP = 1 << 6;

// Alarm, timer and update: flags in FLAG, enables in CTRL0, the same bits.
// Their interrupt line wakes the power controller, so one armed by other
// firmware stays asserted while this one runs, which uses none of them.
constexpr std::uint8_t IRQ_ALARM  = 1 << 3;
constexpr std::uint8_t IRQ_TIMER  = 1 << 4;
constexpr std::uint8_t IRQ_UPDATE = 1 << 5;
constexpr std::uint8_t IRQ_BITS   = IRQ_ALARM | IRQ_TIMER | IRQ_UPDATE;

constexpr std::uint8_t CTRL1_BACKUP = (1 << 4) | (1 << 5);

i2c_master_dev_handle_t s_dev   = nullptr;
bool                    s_valid = false;

esp_err_t read_bytes(std::uint8_t reg, std::uint8_t *out, std::size_t count)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, count, I2C_TIMEOUT_MS);
}

esp_err_t write_byte(std::uint8_t reg, std::uint8_t value)
{
    const std::array<std::uint8_t, 2> tx{reg, value};
    return i2c_master_transmit(s_dev, tx.data(), tx.size(), I2C_TIMEOUT_MS);
}

esp_err_t write_time(const std::tm &utc)
{
    std::array<std::uint8_t, 1 + TIME_FIELDS> tx{REG_SEC};
    std::uint8_t *field = tx.data() + 1;
    field[TIME_SECONDS] = to_bcd(utc.tm_sec);
    field[TIME_MINUTES] = to_bcd(utc.tm_min);
    field[TIME_HOURS]   = to_bcd(utc.tm_hour);
    field[TIME_WEEKDAY] = static_cast<std::uint8_t>(1 << utc.tm_wday);
    field[TIME_DAY]     = to_bcd(utc.tm_mday);
    field[TIME_MONTH]   = to_bcd(utc.tm_mon + 1);
    field[TIME_YEAR]    = to_bcd((utc.tm_year + kTmYearBase) % YEARS_PER_CENTURY);
    return i2c_master_transmit(s_dev, tx.data(), tx.size(), I2C_TIMEOUT_MS);
}

esp_err_t read_time(std::tm &utc)
{
    std::array<std::uint8_t, TIME_FIELDS> field{};
    ESP_RETURN_ON_ERROR(read_bytes(REG_SEC, field.data(), field.size()), TAG, "time registers");
    utc.tm_sec  = from_bcd(field[TIME_SECONDS] & SECONDS_MASK);
    utc.tm_min  = from_bcd(field[TIME_MINUTES] & MINUTES_MASK);
    utc.tm_hour = from_bcd(field[TIME_HOURS] & HOURS_MASK);
    utc.tm_mday = from_bcd(field[TIME_DAY] & DAY_MASK);
    utc.tm_mon  = from_bcd(field[TIME_MONTH] & MONTH_MASK) - 1;
    utc.tm_year = from_bcd(field[TIME_YEAR]) + CHIP_YEAR_BASE - kTmYearBase;
    return ESP_OK;
}

void disarm_leftover_interrupts(std::uint8_t flags)
{
    std::uint8_t ctrl0 = 0;
    const bool   armed = (flags & IRQ_BITS) != 0 ||
                       (read_bytes(REG_CTRL0, &ctrl0, 1) == ESP_OK && (ctrl0 & IRQ_BITS) != 0);
    if (!armed) {
        return;
    }
    ESP_LOGI(TAG, "clearing interrupts left armed (flags 0x%02x, ctrl0 0x%02x)", flags, ctrl0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        write_byte(REG_FLAG, static_cast<std::uint8_t>(flags & ~IRQ_BITS)));
    if (read_bytes(REG_CTRL0, &ctrl0, 1) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            write_byte(REG_CTRL0, static_cast<std::uint8_t>(ctrl0 & ~IRQ_BITS)));
    }
}

}  // namespace

bool holding_time() { return s_valid; }

esp_err_t store(std::time_t when)
{
    ESP_RETURN_ON_FALSE(s_dev != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");

    std::tm utc{};
    if (gmtime_r(&when, &utc) == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    std::uint8_t ctrl0 = 0;
    ESP_RETURN_ON_ERROR(read_bytes(REG_CTRL0, &ctrl0, 1), TAG, "read ctrl0");
    ESP_RETURN_ON_ERROR(write_byte(REG_CTRL0, static_cast<std::uint8_t>(ctrl0 | CTRL0_STOP)), TAG,
                        "stop");

    const esp_err_t err = write_time(utc);

    ESP_RETURN_ON_ERROR(write_byte(REG_CTRL0, static_cast<std::uint8_t>(ctrl0 & ~CTRL0_STOP)), TAG,
                        "start");
    ESP_RETURN_ON_ERROR(err, TAG, "write time");

    std::uint8_t flags = 0;
    if (read_bytes(REG_FLAG, &flags, 1) == ESP_OK && (flags & FLAG_VOLTAGE_LOW) != 0) {
        write_byte(REG_FLAG, static_cast<std::uint8_t>(flags & ~FLAG_VOLTAGE_LOW));
    }
    s_valid = true;
    return ESP_OK;
}

esp_err_t start()
{
    ESP_RETURN_ON_FALSE(s_dev == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_STATE, TAG, "i2c not initialised");

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADDRESS,
        .scl_speed_hz    = I2C_SPEED_HZ,
        .scl_wait_us     = 0,
        .flags           = {},
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");

    std::uint8_t ctrl1 = 0;
    ESP_RETURN_ON_ERROR(read_bytes(REG_CTRL1, &ctrl1, 1), TAG, "read ctrl1");
    ESP_RETURN_ON_ERROR(write_byte(REG_CTRL1, static_cast<std::uint8_t>(ctrl1 | CTRL1_BACKUP)),
                        TAG, "backup supply");

    std::uint8_t flags = 0;
    ESP_RETURN_ON_ERROR(read_bytes(REG_FLAG, &flags, 1), TAG, "read flags");
    disarm_leftover_interrupts(flags);
    if ((flags & FLAG_VOLTAGE_LOW) != 0) {
        ESP_LOGI(TAG, "backup clock lost its time, waiting for the network");
        return ESP_OK;
    }

    std::tm utc{};
    ESP_RETURN_ON_ERROR(read_time(utc), TAG, "read time");

    const std::time_t when = utc_seconds(utc);
    if (!plausible(when)) {
        ESP_LOGW(TAG, "backup clock reads %04d-%02d-%02d, ignoring it", utc.tm_year + kTmYearBase,
                 utc.tm_mon + 1, utc.tm_mday);
        return ESP_OK;
    }

    const timeval now{when, 0};
    settimeofday(&now, nullptr);
    s_valid = true;
    ESP_LOGI(TAG, "clock set from the backup: %04d-%02d-%02d %02d:%02d:%02d UTC",
             utc.tm_year + kTmYearBase, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
             utc.tm_sec);
    return ESP_OK;
}

}  // namespace rtc
