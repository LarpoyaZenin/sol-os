//! 8259A PIC: remap IRQs to vectors 32-47, EOI, and IRQ masking.
//!
//! Faithful port of `kernel/arch/x86_64/pic.c` (C).

const PIC1_CMD: u16 = 0x20;
const PIC1_DATA: u16 = 0x21;
const PIC2_CMD: u16 = 0xA0;
const PIC2_DATA: u16 = 0xA1;

const ICW1_INIT: u8 = 0x10;
const ICW1_ICW4: u8 = 0x01;
const ICW4_8086: u8 = 0x01;

const PIC_EOI: u8 = 0x20;

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

#[inline]
unsafe fn inb(port: u16) -> u8 {
    let val: u8;
    unsafe {
        core::arch::asm!(
            "in al, dx",
            out("al") val,
            in("dx") port,
            options(nomem, nostack, preserves_flags)
        );
    }
    val
}

/// Tiny delay for old hardware that needs a beat between successive
/// PIC I/O writes during the init sequence — writing to an unused
/// port is the traditional OSDev trick for this.
#[inline]
fn io_wait() {
    unsafe { outb(0x80, 0) };
}

/// Remaps master PIC IRQs to vectors 32-39 and slave to 40-47, then
/// masks everything by default — a deterministic starting state rather
/// than inheriting whatever the BIOS/Limine left behind. Individual
/// drivers (timer) unmask their own IRQ line once they're initialized
/// and ready to handle it. Note IRQ2 must stay unmasked on the master
/// PIC since it's the cascade line the slave PIC's IRQs (8-15) depend
/// on. Call after `idt::init`.
pub fn remap() {
    unsafe {
        outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
        io_wait();
        outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
        io_wait();

        outb(PIC1_DATA, 0x20); /* master PIC IRQs start at vector 32 */
        io_wait();
        outb(PIC2_DATA, 0x28); /* slave PIC IRQs start at vector 40 */
        io_wait();

        outb(PIC1_DATA, 4); /* tell master PIC there's a slave at IRQ2 */
        io_wait();
        outb(PIC2_DATA, 2); /* tell slave PIC its cascade identity */
        io_wait();

        outb(PIC1_DATA, ICW4_8086);
        io_wait();
        outb(PIC2_DATA, ICW4_8086);
        io_wait();

        /* Mask everything by default — IRQ2 (cascade) stays unmasked. */
        outb(PIC1_DATA, 0xFB);
        outb(PIC2_DATA, 0xFF);
    }
}

/// Sends EOI to the PIC(s) after servicing `irq`. Always acknowledge
/// the master; the slave only needs one when the IRQ is on the slave.
pub fn send_eoi(irq: u8) {
    unsafe {
        if irq >= 8 {
            outb(PIC2_CMD, PIC_EOI);
        }
        outb(PIC1_CMD, PIC_EOI);
    }
}

/// Unmasks (enables) a single IRQ line — called by driver init
/// functions once their handler is registered and ready.
pub fn unmask_irq(irq: u8) {
    let (port, bit) = if irq < 8 {
        (PIC1_DATA, irq)
    } else {
        (PIC2_DATA, irq - 8)
    };
    unsafe {
        let value = inb(port) & !(1u8 << bit);
        outb(port, value);
    }
}
