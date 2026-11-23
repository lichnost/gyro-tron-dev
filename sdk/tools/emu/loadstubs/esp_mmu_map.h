// Host stub: the exec view is the same memory; nothing here executes it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef uintptr_t esp_paddr_t;
typedef int mmu_target_t;
#define MMU_TARGET_PSRAM0 0
#define MMU_MEM_CAP_EXEC 1
#define MMU_MEM_CAP_READ 2

static inline esp_err_t esp_mmu_vaddr_to_paddr(const void *vaddr, esp_paddr_t *paddr,
                                               mmu_target_t *target)
{
    *paddr = (esp_paddr_t)(uintptr_t)vaddr;
    *target = MMU_TARGET_PSRAM0;
    return ESP_OK;
}

static inline esp_err_t esp_mmu_map(esp_paddr_t paddr, size_t size, mmu_target_t target,
                                    int caps, int split, void **out_handle)
{
    (void)size; (void)target; (void)caps; (void)split;
    *out_handle = (void *)paddr;
    return ESP_OK;
}
static inline esp_err_t esp_mmu_unmap(void *handle) { (void)handle; return ESP_OK; }
