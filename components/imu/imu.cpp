#include "imu.h"

#include "bmi270.h"
#include "bsp/m5stack_tab5.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace imu {
namespace {
constexpr char TAG[] = "imu";

constexpr TickType_t FIRST_SAMPLES_READY = pdMS_TO_TICKS(250);

bmi270_handle_t *s_imu = nullptr;
}  // namespace

esp_err_t start()
{
    ESP_RETURN_ON_FALSE(s_imu == nullptr, ESP_ERR_INVALID_STATE, TAG, "already started");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != nullptr, ESP_ERR_INVALID_STATE, TAG, "i2c not initialised");

    bmi270_driver_config_t driver{};
    driver.addr      = BMI270_I2C_ADDRESS_L;
    driver.interface = BMI270_USE_I2C;
    driver.i2c_bus   = bus;
    ESP_RETURN_ON_ERROR(bmi270_create(&driver, &s_imu), TAG, "create");

    // Slow and gentle: all it has to tell is which way is down.
    const bmi270_config_t config{
        .acce_odr   = BMI270_ACC_ODR_25_HZ,
        .acce_range = BMI270_ACC_RANGE_2_G,
        .gyro_odr   = BMI270_GYR_ODR_25_HZ,
        .gyro_range = BMI270_GYR_RANGE_125_DPS,
    };
    ESP_RETURN_ON_ERROR(bmi270_start(s_imu, &config), TAG, "start");

    vTaskDelay(FIRST_SAMPLES_READY);
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (gravity(x, y, z)) {
        ESP_LOGI(TAG, "gravity %.2f %.2f %.2f g", x, y, z);
    }
    return ESP_OK;
}

bool gravity(float &x, float &y, float &z)
{
    return s_imu != nullptr && bmi270_get_acce_data(s_imu, &x, &y, &z) == ESP_OK;
}

namespace {
// The BMI270's own registers, which its driver keeps to itself: its FIFO.
constexpr std::uint8_t FIFO_LENGTH   = 0x24;
constexpr std::uint8_t FIFO_DATA     = 0x26;
constexpr std::uint8_t FIFO_DOWNS    = 0x45;
constexpr std::uint8_t FIFO_CONFIG_1 = 0x49;
constexpr std::uint8_t CMD           = 0x7E;
constexpr std::uint8_t FIFO_FILTERED = 0x80;  // as the data registers have it, not downsampled
constexpr std::uint8_t FIFO_ACC_ONLY = 0x40;  // no headers: frames of the three axes alone
constexpr std::uint8_t FIFO_FLUSH    = 0xB0;
constexpr std::uint16_t FIFO_LENGTH_MASK = 0x3FFF;
constexpr std::size_t  FIFO_BYTES    = 2048;
constexpr std::size_t  FRAME_BYTES   = 6;
constexpr float        LSB_PER_G     = 16384.0f;  // at the 2 g range

std::int16_t s_last[3]  = {};
bool         s_have_last = false;

esp_err_t write(std::uint8_t reg, std::uint8_t value)
{
    const std::uint8_t out[2] = {reg, value};
    return i2c_master_transmit(s_imu->i2c_handle, out, sizeof(out), -1);
}

esp_err_t read(std::uint8_t reg, std::uint8_t *data, std::size_t length)
{
    return i2c_master_transmit_receive(s_imu->i2c_handle, &reg, 1, data, length, -1);
}
}  // namespace

esp_err_t watch_knocks()
{
    ESP_RETURN_ON_FALSE(s_imu != nullptr, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_ERROR(bmi270_set_acce_odr(s_imu, BMI270_ACC_ODR_800_HZ), TAG, "rate");
    ESP_RETURN_ON_ERROR(write(FIFO_DOWNS, FIFO_FILTERED), TAG, "fifo data");
    ESP_RETURN_ON_ERROR(write(FIFO_CONFIG_1, FIFO_ACC_ONLY), TAG, "fifo");
    ESP_RETURN_ON_ERROR(write(CMD, FIFO_FLUSH), TAG, "flush");
    s_have_last = false;
    return ESP_OK;
}

void stop_watching_knocks()
{
    if (s_imu == nullptr) {
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(write(FIFO_CONFIG_1, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(bmi270_set_acce_odr(s_imu, BMI270_ACC_ODR_25_HZ));
}

float sharpest_jolt()
{
    std::uint8_t length_bytes[2] = {};
    if (s_imu == nullptr || read(FIFO_LENGTH, length_bytes, 2) != ESP_OK) {
        return 0.0f;
    }
    static std::uint8_t frames[FIFO_BYTES];
    std::size_t length = std::min<std::size_t>((length_bytes[0] | (length_bytes[1] << 8)) & FIFO_LENGTH_MASK,
                                               sizeof(frames));
    length -= length % FRAME_BYTES;
    if (length == 0 || read(FIFO_DATA, frames, length) != ESP_OK) {
        return 0.0f;
    }
    float sharpest = 0.0f;
    for (std::size_t at = 0; at < length; at += FRAME_BYTES) {
        std::int16_t axes[3];
        std::memcpy(axes, frames + at, sizeof(axes));
        if (s_have_last) {
            const float dx = (axes[0] - s_last[0]) / LSB_PER_G, dy = (axes[1] - s_last[1]) / LSB_PER_G,
                        dz = (axes[2] - s_last[2]) / LSB_PER_G;
            sharpest = std::max(sharpest, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        std::memcpy(s_last, axes, sizeof(axes));
        s_have_last = true;
    }
    return sharpest;
}



}  // namespace imu
