#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))
#define pdTRUE             1

// One thread: a critical section has nothing to keep out.
typedef struct {
    int unused;
} portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))
