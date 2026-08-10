//! Physical memory manager: a one-bit-per-page bitmap over all of
//! physical memory, tracking which 4 KiB pages are in use.
//!
//! Faithful port of `kernel/mm/pmm.c`: all pages start "used"; the
//! memory map's usable entries flip their pages to "free"; the bitmap
//! itself lives in the first usable region (mapped through the HHDM)
//! and its own pages stay marked used. Allocation is first-fit with a
//! moving hint.
//!
//! Note: Limine has already set up paging (kernel in the higher half,
//! all physical memory mapped at the HHDM offset), so no page tables
//! are touched here — this manager tracks *ownership* of physical
//! frames only.

use crate::boot::requests::{LimineMemmapResponse, LIMINE_MEMMAP_USABLE};
use crate::memory::hhdm::Hhdm;
use crate::memory::PAGE_SIZE;

/// bit set = page in use
static mut PMM_BITMAP: *mut u64 = core::ptr::null_mut();
static mut PMM_TOTAL_PAGES: u64 = 0;
static mut PMM_FIRST_FREE_HINT: u64 = 0;
static mut PMM_FREE_COUNT: u64 = 0;

#[inline]
unsafe fn bitmap_test(page: u64) -> bool {
    let word = unsafe { *PMM_BITMAP.add((page / 64) as usize) };
    (word >> (page % 64)) & 1 != 0
}

#[inline]
unsafe fn bitmap_set(page: u64) {
    let word = unsafe { &mut *PMM_BITMAP.add((page / 64) as usize) };
    *word |= 1u64 << (page % 64);
}

#[inline]
unsafe fn bitmap_clear(page: u64) {
    let word = unsafe { &mut *PMM_BITMAP.add((page / 64) as usize) };
    *word &= !(1u64 << (page % 64));
}

/// Builds the physical page allocator from the bootloader's memory
/// map. `hhdm` is the higher-half direct-map handle. Must be called
/// once, after the Limine requests are populated.
pub fn init(hhdm: &Hhdm, memmap: &LimineMemmapResponse) {
    unsafe {
        if memmap.entries.is_null() {
            crate::kprintln!("[mm] PMM: FATAL no memory map");
            crate::kernel::halt();
        }

        /* Find the highest address described by the map — we track
         * physical pages up to that point. Only *usable* entries count:
         * non-usable entries (BIOS holes, bootloader data, ACPI tables,
         * and the huge reserved ranges some firmware reports at the top
         * of the address space) are never allocated from, and Limine's
         * HHDM mapping does not cover every one of their pages (e.g. the
         * 0xA0000 VGA hole), so walking them would page-fault. */
        let mut highest = 0u64;
        for i in 0..memmap.entry_count as usize {
            let e = &**memmap.entries.add(i);
            if e.type_ != LIMINE_MEMMAP_USABLE {
                continue;
            }
            let end = e.base + e.length;
            if end > highest {
                highest = end;
            }
        }

        PMM_TOTAL_PAGES = (highest + PAGE_SIZE - 1) / PAGE_SIZE;

        let bitmap_bytes = PMM_TOTAL_PAGES / 8;
        let bitmap_pages = (bitmap_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

        /* Place the bitmap at the start of the first usable region. A
         * usable region legitimately starting at physical address 0
         * must not be mistaken for "no usable region", so track success
         * with a flag rather than testing the address itself. */
        let mut bitmap_phys = 0u64;
        let mut bitmap_found = false;
        for i in 0..memmap.entry_count as usize {
            let e = &**memmap.entries.add(i);
            if e.type_ == LIMINE_MEMMAP_USABLE {
                bitmap_phys = e.base;
                bitmap_found = true;
                break;
            }
        }
        if !bitmap_found {
            crate::kprintln!("[mm] PMM: FATAL no usable memory region");
            crate::kernel::halt();
        }

        PMM_BITMAP = hhdm.phys_to_virt(bitmap_phys) as *mut u64;

        /* Start with every tracked page used. */
        let words = (bitmap_bytes + 7) / 8;
        for i in 0..words as usize {
            *PMM_BITMAP.add(i) = !0u64;
        }

        /* Free the pages described as usable by the bootloader. */
        PMM_FREE_COUNT = 0;
        for i in 0..memmap.entry_count as usize {
            let e = &**memmap.entries.add(i);
            if e.type_ != LIMINE_MEMMAP_USABLE {
                continue;
            }
            let start_page = e.base / PAGE_SIZE;
            let mut end_page = (e.base + e.length + PAGE_SIZE - 1) / PAGE_SIZE;
            if end_page > PMM_TOTAL_PAGES {
                end_page = PMM_TOTAL_PAGES;
            }
            for p in start_page..end_page {
                if bitmap_test(p) {
                    /* not double-freeing */
                    bitmap_clear(p);
                    PMM_FREE_COUNT += 1;
                }
            }
        }

        /* Reserve the pages the bitmap itself occupies (first usable
         * region) plus anything below 1 MiB — the bootloader maps / BIOS
         * data and real-mode bounce space live down there. */
        let bitmap_start_page = bitmap_phys / PAGE_SIZE;

        let low_reserve_end = 0x100000 / PAGE_SIZE; /* 1 MiB */
        for p in 0..low_reserve_end {
            if !bitmap_test(p) {
                bitmap_set(p);
                PMM_FREE_COUNT -= 1;
            }
        }
        for p in bitmap_start_page..bitmap_start_page + bitmap_pages {
            if !bitmap_test(p) {
                bitmap_set(p);
                PMM_FREE_COUNT -= 1;
            }
        }

        PMM_FIRST_FREE_HINT = bitmap_start_page + bitmap_pages;

        let total_pages = PMM_TOTAL_PAGES;
        let free_count = PMM_FREE_COUNT;
        crate::kprintln!(
            "[mm] PMM: {} pages total, {} free, bitmap {} pages @ phys {:#x}",
            total_pages,
            free_count,
            bitmap_pages,
            bitmap_phys
        );
    }
}

/// Returns the physical address of one free 4 KiB page, or 0 if out of
/// memory. The returned frame is marked allocated.
pub fn alloc_page() -> u64 {
    unsafe {
        if PMM_BITMAP.is_null() {
            return 0;
        }

        /* Scan from the hint forward, then wrap. */
        for pass in 0..2 {
            let (start, end) = if pass == 0 {
                (PMM_FIRST_FREE_HINT, PMM_TOTAL_PAGES)
            } else {
                (0, PMM_FIRST_FREE_HINT)
            };
            for p in start..end {
                if !bitmap_test(p) {
                    bitmap_set(p);
                    PMM_FREE_COUNT -= 1;
                    PMM_FIRST_FREE_HINT = p + 1;
                    return p * PAGE_SIZE;
                }
            }
        }
    }
    crate::kprintln!("[mm] PMM: out of physical memory");
    0
}

/// Marks the 4 KiB page at physical address `paddr` (must be
/// PAGE_SIZE-aligned) as free. Silently ignores 0 / misaligned.
pub fn free_page(paddr: u64) {
    unsafe {
        if paddr == 0 || paddr % PAGE_SIZE != 0 {
            return;
        }
        let p = paddr / PAGE_SIZE;
        if p >= PMM_TOTAL_PAGES || !bitmap_test(p) {
            return; /* out of range or already free */
        }
        bitmap_clear(p);
        PMM_FREE_COUNT += 1;
        if p < PMM_FIRST_FREE_HINT {
            PMM_FIRST_FREE_HINT = p;
        }
    }
}

/// Number of currently-free 4 KiB pages.
pub fn free_page_count() -> u64 {
    unsafe { PMM_FREE_COUNT }
}

/// Total number of pages tracked by the bitmap.
pub fn total_pages() -> u64 {
    unsafe { PMM_TOTAL_PAGES }
}
