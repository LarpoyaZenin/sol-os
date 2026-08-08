#include "pmm.h"
#include "klog.h"
#include <stddef.h>

/* Physical memory manager: a one-bit-per-page bitmap over all of
 * physical memory, tracking which 4KiB pages are in use. All pages
 * start "used"; the memory map's usable entries flip their pages to
 * "free". The bitmap itself lives in the first usable region (mapped
 * through the HHDM), and its own pages are kept marked as used.
 *
 * Allocation is first-fit with a moving hint, which is simple and
 * good enough until a more sophisticated allocator is needed.
 *
 * Note: Limine has already set up paging (kernel in the higher half,
 * all physical memory mapped at HHDM offset), so no page tables are
 * touched here — this manager tracks *ownership* of physical frames
 * only. */

/* bit set = page in use */
static uint64_t *pmm_bitmap = NULL;
static uint64_t pmm_total_pages = 0;
static uint64_t pmm_first_free_hint = 0;
static uint64_t pmm_free_count = 0;

static inline int bitmap_test(uint64_t page) {
    return (pmm_bitmap[page / 64] >> (page % 64)) & 1u;
}

static inline void bitmap_set(uint64_t page) {
    pmm_bitmap[page / 64] |= (1ull << (page % 64));
}

static inline void bitmap_clear(uint64_t page) {
    pmm_bitmap[page / 64] &= ~(1ull << (page % 64));
}

void pmm_init(const struct limine_memmap_response *memmap, uint64_t hhdm) {
    if (memmap == NULL || memmap->entries == NULL) {
        klog("PMM: FATAL no memory map\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    /* Find the highest address described by the map — we track
     * physical pages up to that point. Only *usable* entries count:
     * non-usable entries (BIOS holes, bootloader data, ACPI tables,
     * and the huge reserved ranges some firmware reports at the top
     * of the address space) are never allocated from, and Limine's
     * HHDM mapping does not cover every one of their pages (e.g. the
     * 0xA0000 VGA hole), so walking them would page-fault. */
    uint64_t highest = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t end = e->base + e->length;
        if (end > highest) highest = end;
    }

    pmm_total_pages = (highest + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t bitmap_bytes = pmm_total_pages / 8;
    uint64_t bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Place the bitmap at the start of the first usable region. A
     * usable region legitimately starting at physical address 0 (which
     * UEFI boot can report, under protocol base revision >= 3) must
     * not be mistaken for "no usable region", so track success with a
     * flag rather than testing the address itself. */
    uintptr_t bitmap_phys = 0;
    int bitmap_found = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            bitmap_phys = e->base;
            bitmap_found = 1;
            break;
        }
    }
    if (!bitmap_found) {
        klog("PMM: FATAL no usable memory region\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    pmm_bitmap = (uint64_t *)(hhdm + bitmap_phys);

    /* Start with every tracked page used. */
    for (uint64_t i = 0; i < (bitmap_bytes + 7) / 8; i++) {
        pmm_bitmap[i] = ~0ull;
    }

    /* Free the pages described as usable by the bootloader. */
    pmm_free_count = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t start_page = e->base / PAGE_SIZE;
        uint64_t end_page = (e->base + e->length + PAGE_SIZE - 1) / PAGE_SIZE;
        if (end_page > pmm_total_pages) end_page = pmm_total_pages;

        for (uint64_t p = start_page; p < end_page; p++) {
            if (!bitmap_test(p)) continue;   /* not double-freeing */
            bitmap_clear(p);
            pmm_free_count++;
        }
    }

    /* Reserve the pages the bitmap itself occupies (first usable
     * region) plus anything below 1MiB — the bootloader maps / BIOS
     * data and real-mode bounce space live down there. */
    uint64_t bitmap_start_page = bitmap_phys / PAGE_SIZE;

    uint64_t low_reserve_end = 0x100000 / PAGE_SIZE;   /* 1 MiB */
    for (uint64_t p = 0; p < low_reserve_end; p++) {
        if (!bitmap_test(p)) { bitmap_set(p); pmm_free_count--; }
    }
    for (uint64_t p = bitmap_start_page; p < bitmap_start_page + bitmap_pages; p++) {
        if (!bitmap_test(p)) { bitmap_set(p); pmm_free_count--; }
    }

    pmm_first_free_hint = bitmap_start_page + bitmap_pages;

    klog("PMM: %lu pages total, %lu free, bitmap %lu pages @ phys %lx\n",
         (unsigned long)pmm_total_pages, (unsigned long)pmm_free_count,
         (unsigned long)bitmap_pages, (unsigned long)bitmap_phys);
}

uintptr_t pmm_alloc_page(void) {
    if (pmm_bitmap == NULL) return 0;

    /* Scan from the hint forward, then wrap. */
    for (uint64_t pass = 0; pass < 2; pass++) {
        uint64_t start = pass == 0 ? pmm_first_free_hint : 0;
        uint64_t end   = pass == 0 ? pmm_total_pages : pmm_first_free_hint;
        for (uint64_t p = start; p < end; p++) {
            if (!bitmap_test(p)) {
                bitmap_set(p);
                pmm_free_count--;
                pmm_first_free_hint = p + 1;
                return p * PAGE_SIZE;
            }
        }
    }
    klog("PMM: out of physical memory\n");
    return 0;
}

void pmm_free_page(uintptr_t paddr) {
    if (paddr == 0 || (paddr % PAGE_SIZE) != 0) return;
    uint64_t p = paddr / PAGE_SIZE;
    if (p >= pmm_total_pages || !bitmap_test(p)) return;  /* out of range or already free */
    bitmap_clear(p);
    pmm_free_count++;
    if (p < pmm_first_free_hint) pmm_first_free_hint = p;
}

uint64_t pmm_free_page_count(void) {
    return pmm_free_count;
}
