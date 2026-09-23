// LVGL's allocations, in PSRAM. Through the C library every allocation of 64
// bytes or less lands in internal memory, and the pages' thousands of style lists
// and label texts took some 190 kB of the pool Wi-Fi, Bluetooth and TLS share.
// The display buffers are allocated by the port, not here.

#include "esp_heap_caps.h"
#include "lvgl.h"

#include <string.h>

#define LV_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_PSRAM_CAPS);
    return p != NULL ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, LV_PSRAM_CAPS);
    return q != NULL ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = heap_caps_get_total_size(LV_PSRAM_CAPS);
    mon_p->free_size  = heap_caps_get_free_size(LV_PSRAM_CAPS);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(LV_PSRAM_CAPS);
    mon_p->used_pct = mon_p->total_size == 0
                          ? 0
                          : (uint8_t)(100 - (100 * mon_p->free_size) / mon_p->total_size);
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(LV_PSRAM_CAPS, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
