#ifndef SOL_PAGING_H
#define SOL_PAGING_H

#include <stdint.h>

/* Maps `len` bytes of physical memory at `hhdm + phys` using 4KiB
 * pages in the page tables currently active (the ones Limine set up).
 * Intermediate page-table levels are allocated from the PMM as
 * needed.
 *
 * Needed because Limine only maps usable RAM through the HHDM offset;
 * device MMIO (PCI BARs) falls in the PCI hole and is otherwise
 * unreachable, which page-faults on access. */
void paging_map_physical(uint64_t hhdm, uint64_t phys, uint64_t len);

#endif /* SOL_PAGING_H */
