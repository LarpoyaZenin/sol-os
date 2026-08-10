//! Interrupt subsystem: GDT, IDT, PIC, and PIT timer.
//!
//! Faithful ports of `kernel/arch/x86_64/{gdt,idt,pic,timer}.c` (C).

pub mod gdt;
pub mod idt;
pub mod pic;
pub mod timer;

/// Sleeps until the next interrupt. The idle loop wakes on every PIT
/// tick and re-checks its heartbeat counter.
#[inline]
pub fn idle() {
    unsafe {
        core::arch::asm!("hlt", options(nomem, nostack, preserves_flags));
    }
}

/// Enables maskable interrupts (STI). Call only after the GDT, IDT,
/// PIC, and all IRQ handlers are set up.
pub unsafe fn enable() {
    unsafe {
        core::arch::asm!("sti", options(nomem, nostack, preserves_flags));
    }
}

/// Disables maskable interrupts (CLI).
pub unsafe fn disable() {
    unsafe {
        core::arch::asm!("cli", options(nomem, nostack, preserves_flags));
    }
}
