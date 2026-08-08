#ifndef SOL_PMM_H
#define SOL_PMM_H

#include <stdint.h>
#include "limine.h"

#define PAGE_SIZE 4096u

/* Builds the physical page allocator from the bootloader's memory
 * map. `hhdm` is the higher-half direct-map offset from the Limine
 * HHDM request — physical addresses become kernel-virtual by adding
 * it. Must be called once, after limine_requests are populated. */
void pmm_init(const struct limine_memmap_response *memmap, uint64_t hhdm);

/* Returns the physical address of one free 4KiB page, or 0 if out of
 * memory. The returned frame is marked allocated. */
uintptr_t pmm_alloc_page(void);

/* Marks the 4KiB page at physical address `paddr` (must be
 * PAGE_SIZE-aligned) as free. Silently ignores 0 / misaligned. */
void pmm_free_page(uintptr_t paddr);

/* Number of currently-free 4KiB pages. */
uint64_t pmm_free_page_count(void);

#endif /* SOL_PMM_H */
