// Host stub: any malloc stands in for PSRAM, and free is a no-op - loader.c hands both
// calloc'd and aligned pointers to heap_caps_free, and this probe exits right after.
#pragma once
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 1

static inline void *heap_caps_malloc(size_t size, int caps) { (void)caps; return malloc(size); }
static inline void *heap_caps_aligned_alloc(size_t align, size_t size, int caps)
{
    (void)align; (void)caps;
    return malloc(size);
}
static inline void heap_caps_free(void *p) { (void)p; }
static inline size_t heap_caps_get_free_size(int caps) { (void)caps; return 0; }
static inline size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return 0; }
