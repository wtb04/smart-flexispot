#pragma once

#include <stdio.h>

#define ESP_LOG_AT(letter, tag, format, ...) printf(letter " (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, format, ...) ESP_LOG_AT("E", tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOG_AT("W", tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOG_AT("I", tag, format, ##__VA_ARGS__)
