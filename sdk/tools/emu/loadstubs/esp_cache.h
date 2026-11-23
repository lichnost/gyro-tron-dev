// Host stub: no cache to sync on the PC.
#pragma once
#include "esp_err.h"

#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
static inline esp_err_t esp_cache_msync(void *addr, size_t size, int flags)
{
    (void)addr; (void)size; (void)flags;
    return ESP_OK;
}
