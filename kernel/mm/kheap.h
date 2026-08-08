#ifndef SOL_KHEAP_H
#define SOL_KHEAP_H

#include <stddef.h>
#include <stdint.h>

/* Carves a contiguous heap out of physical memory (mapped through the
 * HHDM) and installs it as the kernel heap. `hhdm_offset` is the
 * higher-half direct-map offset from the Limine HHDM request. Must be
 * called after pmm_init(). */
void kheap_init(uint64_t hhdm_offset);

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
