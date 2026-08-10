//! PIT (Intel 8254) channel 0 as the 100 Hz timer tick source.
//!
//! Faithful port of `kernel/arch/x86_64/timer.c` (C). Programs PIT
//! channel 0 to fire IRQ0 at ~100 Hz, registers its handler, and
//! unmasks IRQ0.

use core::sync::atomic::{AtomicU64, Ordering};

use crate::interrupts::idt;
use crate::interrupts::pic;

const PIT_CHANNEL0_DATA: u16 = 0x40;
const PIT_COMMAND: u16 = 0x43;
const PIT_BASE_FREQ: u32 = 1193182; /* PIT's fixed input clock, Hz */

static TICKS: AtomicU64 = AtomicU64::new(0);

#[inline]
unsafe fn outb(port: u16, val: u8) {
    unsafe {
        core::arch::asm!(
            "out dx, al",
            in("dx") port,
            in("al") val,
            options(nomem, nostack, preserves_flags)
        );
    }
}

fn timer_handler(_frame: &idt::InterruptFrame) {
    TICKS.fetch_add(1, Ordering::Relaxed);
    pic::send_eoi(0);
}

/// Number of timer ticks since `init` was called.
pub fn ticks() -> u64 {
    TICKS.load(Ordering::Relaxed)
}

/// Programs PIT channel 0 to fire IRQ0 at approximately `hz` times per
/// second, registers its handler, and unmasks IRQ0. Call after
/// `idt::init` and `pic::remap`.
pub fn init(hz: u32) {
    let divisor = PIT_BASE_FREQ / hz;

    /* 0x36 = channel 0, lobyte/hibyte access, mode 3 (square wave),
     * binary (not BCD) counting. */
    unsafe {
        outb(PIT_COMMAND, 0x36);
        outb(PIT_CHANNEL0_DATA, (divisor & 0xFF) as u8);
        outb(PIT_CHANNEL0_DATA, ((divisor >> 8) & 0xFF) as u8);
    }

    idt::register_handler(32, timer_handler); /* IRQ0 -> vector 32 */
    pic::unmask_irq(0);
}
