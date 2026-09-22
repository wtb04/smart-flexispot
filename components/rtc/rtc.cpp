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

constexpr std::uint8_t  ADDRESS      = 0x32;
constexpr std::uint32_t I2C_SPEED_HZ = 400000;
constexpr int           I2C_TIMEOUT_MS = 100;

constexpr std::uint8_t REG_SEC   = 0x10;
constexpr std::uint8_t REG_FLAG  = 0x1d;
constexpr std::uint8_t REG_CTRL0 = 0x1e;
constexpr std::uint8_t REG_CTRL1 = 0x1f;

constexpr std::uint8_t FLAG_VOLTAGE_LOW = 1 << 1;

constexpr std::uint8_t CTRL0_STOP = 1 << 6;

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

    const std::array<std::uint8_t, 8> tx{REG_SEC,
                                         to_bcd(utc.tm_sec),
                                         to_bcd(utc.tm_min),
                                         to_bcd(utc.tm_hour),
                                         static_cast<std::uint8_t>(1 << utc.tm_wday),
                                         to_bcd(utc.tm_mday),
                                         to_bcd(utc.tm_mon + 1),
                                         to_bcd((utc.tm_year + 1900) % 100)};
    const esp_err_t err = i2c_master_transmit(s_dev, tx.data(), tx.size(), I2C_TIMEOUT_MS);

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
    if ((flags & FLAG_VOLTAGE_LOW) != 0) {
        ESP_LOGI(TAG, "backup clock lost its time, waiting for the network");
        return ESP_OK;
    }

    std::array<std::uint8_t, 7> raw{};
    ESP_RETURN_ON_ERROR(read_bytes(REG_SEC, raw.data(), raw.size()), TAG, "read time");

    std::tm utc{};
    utc.tm_sec  = from_bcd(raw[0] & 0x7f);
    utc.tm_min  = from_bcd(raw[1] & 0x7f);
    utc.tm_hour = from_bcd(raw[2] & 0x3f);
    utc.tm_mday = from_bcd(raw[4] & 0x3f);
    utc.tm_mon  = from_bcd(raw[5] & 0x1f) - 1;
    utc.tm_year = from_bcd(raw[6]) + 100;  // the chip counts from 2000, tm from 1900

    const std::time_t when = utc_seconds(utc);
    if (when <= 0) {
        ESP_LOGW(TAG, "backup clock reads %04d-%02d-%02d, ignoring it", utc.tm_year + 1900,
                 utc.tm_mon + 1, utc.tm_mday);
        return ESP_OK;
    }

    const timeval now{when, 0};
    settimeofday(&now, nullptr);
    s_valid = true;
    ESP_LOGI(TAG, "clock set from the backup: %04d-%02d-%02d %02d:%02d:%02d UTC",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    return ESP_OK;
}

}  // namespace rtc
