//! Kernel heap: a first-fit free-list allocator.
//!
//! Faithful port of `kernel/mm/kheap.c` (C). The heap is one
//! contiguous *virtual* range mapped through the HHDM, backed by
//! whatever physical pages the PMM has free (they need not be
//! contiguous). Every block (allocated or free) carries a header
//! describing its payload size, and free blocks additionally link into
//! a doubly-linked list that is kept sorted by address so that freeing
//! can coalesce adjacent blocks. A block is split when an allocation
//! leaves room for at least one more minimum-size block.

use crate::boot::requests::LimineMemmapResponse;
use crate::memory::hhdm::Hhdm;
use crate::memory::paging;
use crate::memory::pmm;
use crate::memory::PAGE_SIZE;

const KHEAP_INITIAL_SIZE: u64 = 16 * 1024 * 1024; /* 16 MiB initial heap */

const HEAP_MAGIC: u32 = 0x48454150; /* "HEAP" */
const HEAP_MIN_BLOCK: usize = 16; /* minimum payload for a split block */

#[repr(C)]
struct HeapBlock {
    size: usize, /* payload bytes, not counting header */
    magic: u32,
    free: u32,
    prev: *mut HeapBlock,
    next: *mut HeapBlock,
}

/* Header + padding to a 16-byte boundary, so every payload is
 * 16-byte aligned (also keeps split blocks 16-byte aligned). */
const BLOCK_HEADER_SIZE: usize = (core::mem::size_of::<HeapBlock>() + 15) & !15;

static mut KHEAP_BASE: *mut u8 = core::ptr::null_mut();
static mut KHEAP_SIZE: u64 = 0;
static mut KHEAP_HEAD: *mut HeapBlock = core::ptr::null_mut();

#[inline]
fn align16(n: usize) -> usize {
    (n + 15) & !15
}

/// Carves a contiguous virtual heap out of physical memory and installs
/// it as the kernel heap. The virtual range sits just above every region
/// the bootloader mapped through the HHDM; each 4 KiB heap page is
/// mapped there individually from a physical page the PMM provides, so
/// the backing pages need not be contiguous. Must be called after
/// `pmm::init`.
pub fn init(hhdm: &Hhdm, memmap: &LimineMemmapResponse) {
    let heap_pages = KHEAP_INITIAL_SIZE / PAGE_SIZE;

    /* The heap needs one contiguous *virtual* range (block allocator
     * uses pointer arithmetic between blocks), but the physical pages
     * behind it don't have to be contiguous — UEFI leaves the map too
     * fragmented for that. So we carve a virtual region above every
     * region Limine maps through the HHDM (Limine maps exactly the
     * memory-map regions, never past their ends) and map each heap
     * page there individually from whatever the PMM hands out. */
    let mut max_end = 0u64;
    for i in 0..memmap.entry_count as usize {
        let e = unsafe { &**memmap.entries.add(i) };
        let end = e.base + e.length;
        if end > max_end {
            max_end = end;
        }
    }
    let virt_off = (max_end + 0x200000 - 1) & !(0x200000 - 1);
    let heap_virt = hhdm.offset() + virt_off;

    for i in 0..heap_pages {
        let pg = pmm::alloc_page();
        if pg == 0 {
            crate::kprintln!(
                "[rust] kheap: FATAL cannot allocate page {}/{}",
                i,
                heap_pages
            );
            crate::kernel::halt();
        }
        unsafe {
            paging::map_physical_at(hhdm, heap_virt + i * PAGE_SIZE, pg, PAGE_SIZE);
        }
    }

    unsafe {
        KHEAP_BASE = heap_virt as *mut u8;
        KHEAP_SIZE = KHEAP_INITIAL_SIZE;

        /* One big free block covering the whole heap. */
        let head = KHEAP_BASE as *mut HeapBlock;
        (*head).size = (KHEAP_SIZE - BLOCK_HEADER_SIZE as u64) as usize;
        (*head).magic = HEAP_MAGIC;
        (*head).free = 1;
        (*head).prev = core::ptr::null_mut();
        (*head).next = core::ptr::null_mut();
        KHEAP_HEAD = head;
    }

    crate::kprintln!(
        "[rust] kheap: {} KiB heap at virt {:#x} (pages above phys max {:#x})",
        KHEAP_INITIAL_SIZE / 1024,
        heap_virt,
        max_end
    );
}

/// Allocates `size` bytes, 16-byte aligned. Returns NULL on OOM.
pub fn kmalloc(size: usize) -> *mut u8 {
    if size == 0 {
        return core::ptr::null_mut();
    }
    let size = align16(size);

    unsafe {
        let mut b = KHEAP_HEAD;
        while !b.is_null() {
            if (*b).free == 0 || (*b).size < size {
                b = (*b).next;
                continue;
            }

            /* Split if there's room for a new minimum block. */
            if (*b).size >= size + BLOCK_HEADER_SIZE + HEAP_MIN_BLOCK {
                let nb = (b as *mut u8).add(BLOCK_HEADER_SIZE + size) as *mut HeapBlock;
                (*nb).size = (*b).size - size - BLOCK_HEADER_SIZE;
                (*nb).magic = HEAP_MAGIC;
                (*nb).free = 1;
                (*nb).prev = b;
                (*nb).next = (*b).next;
                if !(*b).next.is_null() {
                    (*(*b).next).prev = nb;
                }
                (*b).next = nb;
                (*b).size = size;
            }

            (*b).free = 0;
            return (b as *mut u8).add(BLOCK_HEADER_SIZE);
        }
    }

    crate::kprintln!("[rust] kmalloc: out of heap memory (requested {})", size);
    core::ptr::null_mut()
}

/// Allocates and zeroes `n` objects of `size` bytes.
pub fn kcalloc(n: usize, size: usize) -> *mut u8 {
    if n == 0 || size == 0 {
        return core::ptr::null_mut();
    }
    let p = kmalloc(n * size);
    if !p.is_null() {
        unsafe {
            core::ptr::write_bytes(p, 0, n * size);
        }
    }
    p
}

/// Frees a pointer previously returned by `kmalloc`/`kcalloc`. Safe to
/// call with NULL. Coalesces adjacent free blocks.
pub fn kfree(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }

    unsafe {
        let mut b = (ptr as *mut u8).sub(BLOCK_HEADER_SIZE) as *mut HeapBlock;
        if (*b).magic != HEAP_MAGIC {
            crate::kprintln!("[rust] kfree: bad magic at {:p}", ptr);
            return;
        }
        if (*b).free != 0 {
            crate::kprintln!("[rust] kfree: double free of {:p}", ptr);
            return;
        }
        (*b).free = 1;

        /* Coalesce with a free predecessor. */
        if !(*b).prev.is_null() && (*(*b).prev).free != 0 {
            (*(*b).prev).size += BLOCK_HEADER_SIZE + (*b).size;
            (*(*b).prev).next = (*b).next;
            if !(*b).next.is_null() {
                (*(*b).next).prev = (*b).prev;
            }
            b = (*b).prev;
        }

        /* Coalesce with a free successor. */
        if !(*b).next.is_null() && (*(*b).next).free != 0 {
            (*b).size += BLOCK_HEADER_SIZE + (*(*b).next).size;
            (*b).next = (*(*b).next).next;
            if !(*b).next.is_null() {
                (*(*b).next).prev = b;
            }
        }
    }
}

/// Total free bytes in the heap.
pub fn free_bytes() -> u64 {
    let mut total = 0u64;
    unsafe {
        let mut b = KHEAP_HEAD;
        while !b.is_null() {
            if (*b).free != 0 {
                total += (*b).size as u64;
            }
            b = (*b).next;
        }
    }
    total
}

/// Total used (live payload) bytes in the heap.
pub fn used_bytes() -> u64 {
    let mut total = 0u64;
    unsafe {
        let mut b = KHEAP_HEAD;
        while !b.is_null() {
            if (*b).free == 0 {
                total += (*b).size as u64;
            }
            b = (*b).next;
        }
    }
    total
}
