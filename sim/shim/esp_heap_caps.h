// One heap on the desktop: every capability is the C library's.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_EXEC     (1 << 0)
#define MALLOC_CAP_32BIT    (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DMA      (1 << 3)
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT  (1 << 12)

static inline void *heap_caps_malloc(size_t size, unsigned) { return malloc(size); }
static inline void *heap_caps_calloc(size_t n, size_t size, unsigned) { return calloc(n, size); }
static inline void *heap_caps_realloc(void *p, size_t size, unsigned) { return realloc(p, size); }
static inline void  heap_caps_free(void *p) { free(p); }

// As the panel reports itself on a quiet day, for the diagnostics page.
static inline size_t heap_caps_get_total_size(unsigned caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 32u << 20 : 512u << 10;
}
static inline size_t heap_caps_get_free_size(unsigned caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? 24u << 20 : 180u << 10;
}
static inline size_t heap_caps_get_minimum_free_size(unsigned caps)
{
    return heap_caps_get_free_size(caps) / 2;
}
static inline size_t heap_caps_get_largest_free_block(unsigned caps)
{
    return heap_caps_get_free_size(caps) / 4;
}
static inline bool heap_caps_check_integrity(unsigned, bool) { return true; }
