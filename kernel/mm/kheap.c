#include "kheap.h"
#include "pmm.h"
#include "klog.h"
#include "string.h"
#include "limine.h"
#include "arch/x86_64/paging.h"

/* Kernel heap: a first-fit free-list allocator.
 *
 * The heap is one contiguous *virtual* range mapped through the HHDM,
 * backed by whatever physical pages the PMM has free (they need not be
 * contiguous). Every block (allocated or free) carries a header describing
 * its payload size, and free blocks additionally link into a
 * doubly-linked list that is kept sorted by address so that freeing
 * can coalesce adjacent blocks. A block is split when an allocation
 * leaves room for at least one more minimum-size block.
 *
 * This is the roadmap's Stage 2 allocator (free-list, first-fit).
 * Stage 1's bump behavior is trivially recovered by never calling
 * kfree — allocation is then monotonically forward. A slab allocator
 * for fixed-size kernel objects is the planned Stage 3. */

#define KHEAP_SIZE (96u * 1024u * 1024u)   /* 96 MiB initial heap */
/* The heap backs the full-screen backbuffer (8 MiB), the decoded
 * wallpaper bitmap (8 MiB), the FAT32 filesystem image and its
 * scratch buffers, process stacks and kernel objects. */

#define HEAP_MAGIC 0x48454150u           /* "HEAP" */
#define HEAP_MIN_BLOCK 16u                 /* minimum payload for a split block */

struct heap_block {
    size_t size;                 /* payload bytes, not counting header */
    uint32_t magic;
    int free;
    struct heap_block *prev;
    struct heap_block *next;
};

/* Header + padding to a 16-byte boundary, so every payload is
 * 16-byte aligned (also keeps split blocks 16-byte aligned). */
#define BLOCK_HEADER_SIZE ((sizeof(struct heap_block) + 15u) & ~15ull)

static uint8_t *kheap_base = NULL;
static uint64_t kheap_size = 0;
static struct heap_block *kheap_head = NULL;

static inline size_t align16(size_t n) {
    return (n + 15u) & ~(size_t)15u;
}

void kheap_init(uint64_t hhdm_offset, const struct limine_memmap_response *memmap) {
    uint64_t heap_pages = KHEAP_SIZE / PAGE_SIZE;

    /* The heap needs one contiguous *virtual* range (block allocator
     * uses pointer arithmetic between blocks), but the physical pages
     * behind it don't have to be contiguous — UEFI leaves the map too
     * fragmented for that. So we carve a virtual region above every
     * region Limine maps through the HHDM (Limine maps exactly the
     * memory-map regions, never past their ends) and map each heap
     * page there individually from whatever the PMM hands out. */
    uint64_t max_end = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        uint64_t end = memmap->entries[i]->base + memmap->entries[i]->length;
        if (end > max_end) max_end = end;
    }
    uint64_t virt_off = (max_end + 0x200000ull - 1) & ~(0x200000ull - 1);
    uint64_t heap_virt = hhdm_offset + virt_off;

    for (uint64_t i = 0; i < heap_pages; i++) {
        uintptr_t pg = pmm_alloc_page();
        if (pg == 0) {
            klog("kheap: FATAL cannot allocate page %lu/%lu\n",
                 (unsigned long)i, (unsigned long)heap_pages);
            for (;;) { __asm__ volatile ("cli; hlt"); }
        }
        paging_map_physical_at(hhdm_offset, heap_virt + i * PAGE_SIZE, pg, PAGE_SIZE);
    }

    kheap_base = (uint8_t *)heap_virt;
    kheap_size = KHEAP_SIZE;

    /* One big free block covering the whole heap. */
    kheap_head = (struct heap_block *)kheap_base;
    kheap_head->size = kheap_size - BLOCK_HEADER_SIZE;
    kheap_head->magic = HEAP_MAGIC;
    kheap_head->free = 1;
    kheap_head->prev = NULL;
    kheap_head->next = NULL;

    klog("kheap: %lu KiB heap at virt %p (pages above phys max %lx)\n",
         (unsigned long)(KHEAP_SIZE / 1024), (void *)kheap_base,
         (unsigned long)max_end);
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = align16(size);

    for (struct heap_block *b = kheap_head; b != NULL; b = b->next) {
        if (!b->free || b->size < size) continue;

        /* Split if there's room for a new minimum block. */
        if (b->size >= size + BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK) {
            struct heap_block *nb = (struct heap_block *)((uint8_t *)b + BLOCK_HEADER_SIZE + size);
            nb->size   = b->size - size - BLOCK_HEADER_SIZE;
            nb->magic  = HEAP_MAGIC;
            nb->free   = 1;
            nb->prev   = b;
            nb->next   = b->next;
            if (b->next != NULL) b->next->prev = nb;
            b->next = nb;
            b->size = size;
        }

        b->free = 0;
        return (void *)((uint8_t *)b + BLOCK_HEADER_SIZE);
    }

    klog("kmalloc: out of heap memory (requested %lu)\n", (unsigned long)size);
    return NULL;
}

void *kcalloc(size_t n, size_t size) {
    if (n == 0 || size == 0) return NULL;
    void *p = kmalloc(n * size);
    if (p != NULL) memset(p, 0, n * size);
    return p;
}

void kfree(void *ptr) {
    if (ptr == NULL) return;

    struct heap_block *b = (struct heap_block *)((uint8_t *)ptr - BLOCK_HEADER_SIZE);
    if (b->magic != HEAP_MAGIC) {
        klog("kfree: bad magic at %p\n", ptr);
        return;
    }
    if (b->free) {
        klog("kfree: double free of %p\n", ptr);
        return;
    }
    b->free = 1;

    /* Coalesce with a free predecessor. */
    if (b->prev != NULL && b->prev->free) {
        b->prev->size += BLOCK_HEADER_SIZE + b->size;
        b->prev->next = b->next;
        if (b->next != NULL) b->next->prev = b->prev;
        b = b->prev;
    }

    /* Coalesce with a free successor. */
    if (b->next != NULL && b->next->free) {
        b->size += BLOCK_HEADER_SIZE + b->next->size;
        b->next = b->next->next;
        if (b->next != NULL) b->next->prev = b;
    }
}

uint64_t kheap_free_bytes(void) {
    uint64_t total = 0;
    for (struct heap_block *b = kheap_head; b != NULL; b = b->next) {
        if (b->free) total += b->size;
    }
    return total;
}

uint64_t kheap_used_bytes(void) {
    uint64_t total = 0;
    for (struct heap_block *b = kheap_head; b != NULL; b = b->next) {
        if (!b->free) total += b->size;
    }
    return total;
}
