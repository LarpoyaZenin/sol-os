//! PS/2 keyboard and mouse drivers (standard 8042 protocol).
//!
//! Faithful ports of `kernel/arch/x86_64/keyboard.c` and `mouse.c`
//! (C). Unlike the C versions — which converted scancodes to
//! unshifted characters immediately and dropped every break code —
//! both drivers here forward every physical make AND break to the
//! central input subsystem, which owns modifier tracking and the
//! keycode-to-character layout.
//!
//! QEMU's 8042 sends the keyboard in AT scancode set 1 (the mouse
//! init explicitly disables scancode translation), so keyboard codes
//! here already match the shared Linux/AT-set-1 namespace.

use crate::input;
use crate::interrupts::{idt, pic};

const PS2_DATA: u16 = 0x60;
const PS2_STATUS: u16 = 0x64;

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

/* ---- keyboard (IRQ1 -> vector 33) ---- */

/// Set when the last byte was the 0xE0 extended-key prefix; the next
/// byte's code is tagged so it can't collide with the typing area.
static mut EXTENDED: bool = false;

fn keyboard_handler(_frame: &idt::InterruptFrame) {
    let scancode = unsafe { inb(PS2_DATA) };

    if scancode == 0xE0 {
        unsafe { EXTENDED = true };
        pic::send_eoi(1);
        return;
    }

    let extended = unsafe { EXTENDED };
    unsafe { EXTENDED = false };
    let code = if extended {
        0x100 + (scancode & 0x7F) as u16
    } else {
        (scancode & 0x7F) as u16
    };

    /* Set-1 break codes are the make code with bit 7 set. */
    input::submit_key(code, scancode & 0x80 == 0);

    pic::send_eoi(1);
}

pub fn keyboard_init() {
    idt::register_handler(33, keyboard_handler); /* IRQ1 -> vector 33 */
    pic::unmask_irq(1);
    crate::kprintln!("[rust] PS/2 keyboard initialized");
}

/* ---- mouse (IRQ12 -> vector 44) ---- */

static mut MOUSE_PACKET: [u8; 3] = [0; 3];
static mut MOUSE_CYCLE: u8 = 0;
static mut LAST_BUTTONS: u8 = 0;

fn mouse_handler(_frame: &idt::InterruptFrame) {
    let byte = unsafe { inb(PS2_DATA) };

    unsafe {
        if MOUSE_CYCLE == 0 {
            /* Sync byte: bit 3 must be set, else we're mid-packet. */
            if byte & 0x08 == 0 {
                pic::send_eoi(12);
                return;
            }
            MOUSE_PACKET[0] = byte;
            MOUSE_CYCLE = 1;
        } else if MOUSE_CYCLE == 1 {
            MOUSE_PACKET[1] = byte;
            MOUSE_CYCLE = 2;
        } else {
            MOUSE_PACKET[2] = byte;
            MOUSE_CYCLE = 0;

            let b0 = MOUSE_PACKET[0];
            let mut dx = MOUSE_PACKET[1] as i8 as i32;
            let mut dy = MOUSE_PACKET[2] as i8 as i32;

            /* Overflow flags in byte 0: bit 6 = x overflow, bit 7 = y. */
            if b0 & 0x40 != 0 {
                dx = 0;
            }
            if b0 & 0x80 != 0 {
                dy = 0;
            }

            /* Buttons (bits 0-2 = left/right/middle) are a bitmask
             * per packet; convert transitions to press/release events. */
            let btns = b0 & 0x07;
            let buttons = [
                input::MouseButton::Left,
                input::MouseButton::Right,
                input::MouseButton::Middle,
            ];
            for i in 0..buttons.len() {
                let now = btns & (1u8 << i) != 0;
                let was = LAST_BUTTONS & (1u8 << i) != 0;
                if now != was {
                    input::submit_mouse_button(buttons[i], now);
                }
            }
            LAST_BUTTONS = btns;

            input::submit_mouse_move(dx, dy);
        }
    }

    pic::send_eoi(12);
}

/* ---- 8042 controller plumbing (ported from mouse.c) ---- */

/// True once the controller's input buffer is empty (we can send).
fn ps2_wait_input() -> bool {
    for _ in 0..100000 {
        if unsafe { inb(PS2_STATUS) } & 0x02 == 0 {
            return true;
        }
    }
    false
}

/// True once the controller's output buffer has data (we can read).
fn ps2_wait_output() -> bool {
    for _ in 0..100000 {
        if unsafe { inb(PS2_STATUS) } & 0x01 != 0 {
            return true;
        }
    }
    false
}

fn ps2_flush() {
    while unsafe { inb(PS2_STATUS) } & 0x01 != 0 {
        let _ = unsafe { inb(PS2_DATA) };
    }
}

/// Sends a controller command (port 0x64).
fn ps2_send(cmd: u8) -> bool {
    if !ps2_wait_input() {
        return false;
    }
    unsafe { outb(PS2_STATUS, cmd) };
    true
}

/// Sends a byte to the auxiliary (mouse) device: 0xD4 controller
/// command first, then the data byte.
fn ps2_write_mouse(b: u8) -> bool {
    if !ps2_wait_input() {
        return false;
    }
    unsafe { outb(PS2_STATUS, 0xD4) };
    if !ps2_wait_input() {
        return false;
    }
    unsafe { outb(PS2_DATA, b) };
    true
}

/// Writes a plain byte to port 0x60 (used after the 0x60 command).
fn ps2_write_device(b: u8) -> bool {
    if !ps2_wait_input() {
        return false;
    }
    unsafe { outb(PS2_DATA, b) };
    true
}

/// Reads a byte from a device (ACK / response).
fn ps2_read_device(out: &mut u8) -> bool {
    if !ps2_wait_output() {
        return false;
    }
    *out = unsafe { inb(PS2_DATA) };
    true
}

pub fn mouse_init() {
    let mut d: u8 = 0;

    /* Disable both devices while we reconfigure. */
    ps2_send(0xAD);
    ps2_wait_input();
    ps2_send(0xA7);
    ps2_wait_input();
    ps2_flush();

    /* Read the controller command byte; enable both interrupts and
     * clocks, and disable scancode translation (raw set 1). */
    if ps2_send(0x20) && ps2_read_device(&mut d) {
        d |= 0x01; /* keyboard IRQ */
        d |= 0x02; /* mouse IRQ */
        d |= 0x04; /* system flag */
        d &= !0x10; /* enable keyboard clock */
        d &= !0x20; /* enable mouse clock */
        d &= !0x40; /* no scancode translation */
        ps2_send(0x60);
        ps2_write_device(d);
    }

    /* Enable the auxiliary device. */
    ps2_send(0xA8);
    ps2_wait_input();

    /* Set defaults, then enable data reporting. */
    if ps2_write_mouse(0xF6) {
        let _ = ps2_read_device(&mut d); /* ACK */
    }
    if ps2_write_mouse(0xF4) {
        let _ = ps2_read_device(&mut d); /* ACK */
    }

    /* Re-enable the keyboard. */
    ps2_send(0xAE);
    ps2_wait_input();

    idt::register_handler(44, mouse_handler); /* IRQ12 -> vector 44 */
    pic::unmask_irq(12);
    crate::kprintln!("[rust] PS/2 mouse initialized");
}
