#include "paging.h"
#include "mm/pmm.h"
#include "klog.h"
#include <stddef.h>

/* Paging constants (x86-64 4-level). */
#define PAGE_SIZE 4096u
#define PAGE_INDEX_SHIFT(virt, shift) (((virt) >> (shift)) & 0x1FFull)

#define PAGE_PRESENT  0x001ull
#define PAGE_WRITABLE 0x002ull
#define PAGE_HUGE     0x080ull

/* Clears bits 0-11 (flags) and 52-63 (NX etc.), leaving the frame. */
#define PTE_FRAME(entry) ((entry) & 0x000FFFFFFFFFF000ull)

static inline uint64_t paging_read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static inline void paging_invlpg(uintptr_t vaddr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(vaddr) : "memory");
}

/* Allocates and zeroes one page-table page from the PMM. Returns its
 * physical address, or 0 on failure. */
static uintptr_t paging_new_table(uint64_t hhdm) {
    uintptr_t phys = pmm_alloc_page();
    if (phys == 0) {
        klog("paging: out of memory for page tables\n");
        return 0;
    }
    uint64_t *tab = (uint64_t *)(hhdm + phys);
    for (int i = 0; i < 512; i++) tab[i] = 0;
    return phys;
}

void paging_map_physical(uint64_t hhdm, uint64_t phys, uint64_t len) {
    uint64_t *pml4 = (uint64_t *)(hhdm + PTE_FRAME(paging_read_cr3()));

    uint64_t start = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = phys + len;

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t vaddr = hhdm + page;
        uint64_t pml4_i = PAGE_INDEX_SHIFT(vaddr, 39);
        uint64_t pdpt_i = PAGE_INDEX_SHIFT(vaddr, 30);
        uint64_t pd_i   = PAGE_INDEX_SHIFT(vaddr, 21);
        uint64_t pt_i   = PAGE_INDEX_SHIFT(vaddr, 12);

        if (!(pml4[pml4_i] & PAGE_PRESENT)) {
            uintptr_t np = paging_new_table(hhdm);
            if (np == 0) return;
            pml4[pml4_i] = np | PAGE_PRESENT | PAGE_WRITABLE;
        }
        uint64_t *pdpt = (uint64_t *)(hhdm + PTE_FRAME(pml4[pml4_i]));
        if (pdpt[pdpt_i] & (PAGE_PRESENT | PAGE_HUGE)) {
            if (pdpt[pdpt_i] & PAGE_HUGE) continue;   /* 1 GiB page already covers it */
        } else {
            uintptr_t np = paging_new_table(hhdm);
            if (np == 0) return;
            pdpt[pdpt_i] = np | PAGE_PRESENT | PAGE_WRITABLE;
        }
        uint64_t *pd = (uint64_t *)(hhdm + PTE_FRAME(pdpt[pdpt_i]));
        if (pd[pd_i] & (PAGE_PRESENT | PAGE_HUGE)) {
            if (pd[pd_i] & PAGE_HUGE) continue;       /* 2 MiB page already covers it */
        } else {
            uintptr_t np = paging_new_table(hhdm);
            if (np == 0) return;
            pd[pd_i] = np | PAGE_PRESENT | PAGE_WRITABLE;
        }
        uint64_t *pt = (uint64_t *)(hhdm + PTE_FRAME(pd[pd_i]));
        pt[pt_i] = page | PAGE_PRESENT | PAGE_WRITABLE;
        paging_invlpg(vaddr);
    }
}
