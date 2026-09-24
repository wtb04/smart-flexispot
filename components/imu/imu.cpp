#include "imu.h"

#include "bmi270.h"
#include "bsp/m5stack_tab5.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace imu {
namespace {
constexpr char TAG[] = "imu";

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

    vTaskDelay(pdMS_TO_TICKS(250));  // the first samples are not ready at once
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

}  // namespace imu
