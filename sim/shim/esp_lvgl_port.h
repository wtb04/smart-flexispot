// The simulator draws on its one thread, so the lock is never contended.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

static inline bool lvgl_port_lock(uint32_t) { return true; }
static inline void lvgl_port_unlock(void) {}
