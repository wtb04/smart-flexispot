// Stand-ins for the parts of ESP-IDF that components/ui touches, so it builds
// on the desktop unchanged.
#pragma once

#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL              -1
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE  0x104
#define ESP_ERR_NOT_FOUND     0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT       0x107

static inline const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
        case ESP_OK:                return "ESP_OK";
        case ESP_FAIL:              return "ESP_FAIL";
        case ESP_ERR_NO_MEM:        return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG:   return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE:  return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND:     return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_TIMEOUT:       return "ESP_ERR_TIMEOUT";
        default:                    return "ESP_ERR";
    }
}

#define ESP_ERROR_CHECK(x)                                                            \
    do {                                                                              \
        esp_err_t err_ = (x);                                                         \
        if (err_ != ESP_OK) {                                                         \
            fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__, __LINE__, #x,         \
                    esp_err_to_name(err_));                                           \
            abort();                                                                  \
        }                                                                             \
    } while (0)

#define ESP_ERROR_CHECK_WITHOUT_ABORT(x)                                              \
    ({                                                                                \
        esp_err_t err_ = (x);                                                         \
        if (err_ != ESP_OK) {                                                         \
            fprintf(stderr, "%s:%d: %s failed: %s\n", __FILE__, __LINE__, #x,         \
                    esp_err_to_name(err_));                                           \
        }                                                                             \
        err_;                                                                         \
    })
