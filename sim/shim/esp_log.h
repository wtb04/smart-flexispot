#pragma once

#include <stdio.h>

typedef enum { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_VERBOSE } esp_log_level_t;

#define ESP_LOG_AT(letter, tag, format, ...) printf(letter " (%s) " format "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, format, ...) ESP_LOG_AT("E", tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) ESP_LOG_AT("W", tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) ESP_LOG_AT("I", tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) ((void)(tag))
#define ESP_LOGV(tag, format, ...) ((void)(tag))

static inline void esp_log_level_set(const char *, esp_log_level_t) {}
