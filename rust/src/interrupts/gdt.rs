//! Global Descriptor Table: null, kernel code, kernel data.
//!
//! Faithful port of `kernel/arch/x86_64/gdt.c` (C). Sets up and loads
//! Sol OS's own 3-entry GDT (null, kernel code, kernel data). Must run
//! before `idt::init`, since interrupt handlers rely on a known-good
//! code segment selector — Limine's GDT is only guaranteed valid up
//! until the kernel takes over. No TSS yet; that arrives with ring 3 /
//! user mode in Phase 4.

use core::arch::global_asm;

global_asm!(include_str!("gdt_flush.s"));

/// Packed GDT entry — 8 bytes, standard x86 layout. In long mode most
/// of the base/limit fields are ignored by the CPU (segmentation is
/// effectively flat), but they must still be encoded correctly for the
/// descriptor to be considered valid.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct GdtEntry {
    limit_low: u16,
    base_low: u16,
    base_mid: u8,
    access: u8,
    granularity: u8,
    base_high: u8,
}

/// Packed GDT pointer handed to `lgdt`.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct GdtPtr {
    limit: u16,
    base: u64,
}

const GDT_ENTRIES: usize = 3;

static mut GDT: [GdtEntry; GDT_ENTRIES] = [GdtEntry {
    limit_low: 0,
    base_low: 0,
    base_mid: 0,
    access: 0,
    granularity: 0,
    base_high: 0,
}; GDT_ENTRIES];

static mut GDTP: GdtPtr = GdtPtr { limit: 0, base: 0 };

/* Defined in `gdt_flush.s`: loads `gdtp` via lgdt, then far-returns to
 * reload CS with the new code selector and reloads the data segment
 * registers with the new data selector. */
extern "C" {
    fn gdt_flush(gdtp_addr: *const GdtPtr);
}

fn set_entry(i: usize, base: u32, limit: u32, access: u8, gran: u8) {
    unsafe {
        GDT[i].base_low = (base & 0xFFFF) as u16;
        GDT[i].base_mid = ((base >> 16) & 0xFF) as u8;
        GDT[i].base_high = ((base >> 24) & 0xFF) as u8;
        GDT[i].limit_low = (limit & 0xFFFF) as u16;
        GDT[i].granularity = ((limit >> 16) & 0x0F) as u8 | (gran & 0xF0);
        GDT[i].access = access;
    }
}

/// Builds and loads the 3-entry GDT. Call before `idt::init`.
pub fn init() {
    unsafe {
        GDTP.limit = (core::mem::size_of::<GdtEntry>() * GDT_ENTRIES - 1) as u16;
        GDTP.base = core::ptr::addr_of!(GDT) as u64;

        /* Null descriptor — required by the architecture. */
        set_entry(0, 0, 0, 0, 0);

        /* Kernel code: base 0, limit ignored in long mode (using
         * 0xFFFFF here per convention), access 0x9A = present, ring 0,
         * code segment, executable, readable. granularity 0xAF sets
         * the long-mode (L) bit plus 4KiB granularity. */
        set_entry(1, 0, 0xFFFFF, 0x9A, 0xAF);

        /* Kernel data: access 0x92 = present, ring 0, data segment,
         * writable. granularity 0xCF = 4KiB granularity, 32-bit (data
         * segments don't use the L bit). */
        set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);

        gdt_flush(core::ptr::addr_of!(GDTP));
    }
}
