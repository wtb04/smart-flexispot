#pragma once

#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_FALSE(a, err_code, log_tag, format, ...)                        \
    do {                                                                              \
        if (!(a)) {                                                                   \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);  \
            return err_code;                                                          \
        }                                                                             \
    } while (0)

#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...)                                  \
    do {                                                                              \
        esp_err_t err_rc_ = (x);                                                      \
        if (err_rc_ != ESP_OK) {                                                      \
            ESP_LOGE(log_tag, "%s(%d): " format, __func__, __LINE__, ##__VA_ARGS__);  \
            return err_rc_;                                                           \
        }                                                                             \
    } while (0)
