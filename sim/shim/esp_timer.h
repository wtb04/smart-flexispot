#pragma once

#include <stdint.h>

// Microseconds since the simulator started, as esp_timer counts from boot.
int64_t esp_timer_get_time(void);
