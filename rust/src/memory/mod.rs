//! Memory subsystem: physical memory manager, kernel heap, paging, and
//! the HHDM abstraction. Faithful ports of kernel/mm/{pmm,kheap}.c and
//! kernel/arch/x86_64/paging.c (C).

pub mod hhdm;
pub mod kheap;
pub mod paging;
pub mod pmm;
pub mod selftest;

use crate::boot::requests::LimineMemmapResponse;
use hhdm::Hhdm;

/// Physical page size (4 KiB).
pub const PAGE_SIZE: u64 = 4096;

/// Initializes the physical memory manager and kernel heap from the
/// Limine memory map, then runs the memory self-tests.
pub fn init(hhdm: &Hhdm, memmap: &LimineMemmapResponse) {
    pmm::init(hhdm, memmap);
    crate::kprintln!("[rust] PMM initialized");

    kheap::init(hhdm, memmap);
    crate::kprintln!("[rust] kheap initialized");

    selftest::run();
}
