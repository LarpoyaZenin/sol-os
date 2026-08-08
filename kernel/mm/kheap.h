#ifndef SOL_KHEAP_H
#define SOL_KHEAP_H

#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

/* Carves a contiguous virtual heap out of physical memory and installs
 * it as the kernel heap. The virtual range sits just above every region
 * the bootloader mapped through the HHDM; each 4KiB heap page is mapped
 * there individually from a physical page the PMM provides, so the
 * backing pages need not be contiguous. `hhdm_offset` is the Limine
 * HHDM offset; `memmap` its memory-map response. Must be called after
 * pmm_init(). */
void kheap_init(uint64_t hhdm_offset, const struct limine_memmap_response *memmap);

/* Allocates `size` bytes, 16-byte aligned. Returns NULL on OOM. */
void *kmalloc(size_t size);

/* Allocates and zeroes `n` objects of `size` bytes. */
void *kcalloc(size_t n, size_t size);

/* Frees a pointer previously returned by kmalloc/kcalloc. Safe to
 * call with NULL. Coalesces adjacent free blocks. */
void kfree(void *ptr);

/* Total free and used (live payload) bytes in the heap. */
uint64_t kheap_free_bytes(void);
uint64_t kheap_used_bytes(void);

#endif /* SOL_KHEAP_H */
