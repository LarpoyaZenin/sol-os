//! x86-64 4-level paging helpers: maps physical ranges at virtual
//! addresses, building page-table pages from the PMM on demand.
//! Faithful port of `kernel/arch/x86_64/paging.c` (C).
//!
//! Limine has already set up paging and the HHDM mapping; this code
//! only *adds* new 4 KiB mappings on top of the existing tables.

use crate::memory::hhdm::Hhdm;
use crate::memory::pmm;
use crate::memory::PAGE_SIZE;

const PAGE_PRESENT: u64 = 0x001;
const PAGE_WRITABLE: u64 = 0x002;
const PAGE_HUGE: u64 = 0x080;

/// Clears bits 0-11 (flags) and 52-63 (NX etc.), leaving the frame.
const PTE_FRAME_MASK: u64 = 0x000F_FFFF_FFFF_F000;

#[inline]
fn page_index_shift(virt: u64, shift: u64) -> usize {
    ((virt >> shift) & 0x1FF) as usize
}

#[inline]
fn read_cr3() -> u64 {
    // Intel-syntax move: `mov reg, cr3` reads CR3 into the output.
    // (The operands in `mov cr3, {}` would be reversed under Intel
    // syntax, writing garbage into CR3 — this target's inline asm
    // dialect is Intel.)
    let cr3: u64;
    unsafe {
        core::arch::asm!("mov {}, cr3", out(reg) cr3, options(nomem, nostack, preserves_flags));
    }
    cr3
}

#[inline]
fn invlpg(vaddr: u64) {
    // Intel-syntax memory operand: `invlpg [reg]`. (The AT&T
    // `(reg)` form is rejected by LLVM because it can render rbp/r13,
    // which are invalid as base registers without a displacement.)
    unsafe {
        core::arch::asm!(
            "invlpg [{}]",
            in(reg) vaddr,
            options(nostack, preserves_flags)
        );
    }
}

/// Allocates and zeroes one page-table page from the PMM. Returns its
/// physical address, or 0 on failure.
fn new_table(hhdm: &Hhdm) -> u64 {
    let phys = pmm::alloc_page();
    if phys == 0 {
        crate::kprintln!("[rust] paging: out of memory for page tables");
        return 0;
    }
    let tab = hhdm.phys_to_virt(phys) as *mut u64;
    for i in 0..512 {
        unsafe {
            *tab.add(i) = 0;
        }
    }
    phys
}

/// Maps the physical range [phys, phys+len) at virtual addresses
/// [vaddr, vaddr+len) using 4 KiB pages. `vaddr` must be aligned to
/// the start page of `phys`. Shared core of `map_physical()` and
/// `map_physical_at()`.
pub unsafe fn map_physical_at(hhdm: &Hhdm, vaddr: u64, phys: u64, len: u64) {
    let pml4 = hhdm.phys_to_virt(read_cr3() & PTE_FRAME_MASK) as *mut u64;

    let start = phys & !(PAGE_SIZE - 1);
    let end = phys + len;
    let mut page = start;
    let mut v = vaddr;

    while page < end {
        let pml4_i = page_index_shift(v, 39);
        let pdpt_i = page_index_shift(v, 30);
        let pd_i = page_index_shift(v, 21);
        let pt_i = page_index_shift(v, 12);

        if unsafe { *pml4.add(pml4_i) } & PAGE_PRESENT == 0 {
            let np = new_table(hhdm);
            if np == 0 {
                return;
            }
            unsafe {
                *pml4.add(pml4_i) = np | PAGE_PRESENT | PAGE_WRITABLE;
            }
        }

        let pdpt = hhdm.phys_to_virt(unsafe { *pml4.add(pml4_i) } & PTE_FRAME_MASK) as *mut u64;
        let pdpt_entry = unsafe { *pdpt.add(pdpt_i) };
        if pdpt_entry & PAGE_HUGE != 0 {
            /* 1 GiB page already covers this page */
            page += PAGE_SIZE;
            v += PAGE_SIZE;
            continue;
        }
        if pdpt_entry & PAGE_PRESENT == 0 {
            let np = new_table(hhdm);
            if np == 0 {
                return;
            }
            unsafe {
                *pdpt.add(pdpt_i) = np | PAGE_PRESENT | PAGE_WRITABLE;
            }
        }

        let pd = hhdm.phys_to_virt(unsafe { *pdpt.add(pdpt_i) } & PTE_FRAME_MASK) as *mut u64;
        let pd_entry = unsafe { *pd.add(pd_i) };
        if pd_entry & PAGE_HUGE != 0 {
            /* 2 MiB page already covers this page */
            page += PAGE_SIZE;
            v += PAGE_SIZE;
            continue;
        }
        if pd_entry & PAGE_PRESENT == 0 {
            let np = new_table(hhdm);
            if np == 0 {
                return;
            }
            unsafe {
                *pd.add(pd_i) = np | PAGE_PRESENT | PAGE_WRITABLE;
            }
        }

        let pt = hhdm.phys_to_virt(unsafe { *pd.add(pd_i) } & PTE_FRAME_MASK) as *mut u64;
        unsafe {
            *pt.add(pt_i) = page | PAGE_PRESENT | PAGE_WRITABLE;
            invlpg(v);
        }

        page += PAGE_SIZE;
        v += PAGE_SIZE;
    }
}

/// Maps the physical range [phys, phys+len) at the HHDM alias
/// (`phys` as a kernel-virtual address), i.e. identity-in-HHDM.
pub unsafe fn map_physical(hhdm: &Hhdm, phys: u64, len: u64) {
    unsafe {
        map_physical_at(
            hhdm,
            hhdm.offset() + (phys & !(PAGE_SIZE - 1)),
            phys,
            len,
        );
    }
}
