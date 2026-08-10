//! Safe HHDM (higher-half direct map) abstraction.
//!
//! Limine maps all usable physical memory at a fixed higher-half
//! offset: physical address `p` is readable/writable at `p + offset`
//! in the kernel's address space. All physical<->virtual conversions
//! go through this type so the raw arithmetic stays in one place.

/// HHDM handle: `phys_to_virt(p) == p + offset`.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Hhdm {
    offset: u64,
}

impl Hhdm {
    /// Builds the handle from the Limine HHDM response offset.
    pub fn from_offset(offset: u64) -> Hhdm {
        Hhdm { offset }
    }

    pub fn offset(&self) -> u64 {
        self.offset
    }

    /// Physical address -> kernel-virtual address (adds the offset).
    pub fn phys_to_virt(&self, phys: u64) -> usize {
        (phys + self.offset) as usize
    }

    /// Kernel-virtual HHDM address -> physical address (subtracts the
    /// offset). Precondition: `virt` lies inside the HHDM range.
    pub fn virt_to_phys(&self, virt: usize) -> u64 {
        (virt as u64) - self.offset
    }
}
