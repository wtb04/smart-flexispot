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
