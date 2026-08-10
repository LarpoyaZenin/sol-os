//! Kernel core: serial logging and the fatal halt loop.

pub mod serial;

/// Fatal stop: disable interrupts and halt forever. Used by the panic
/// handler and fatal boot-path failures; never returns.
pub fn halt() -> ! {
    loop {
        unsafe {
            core::arch::asm!("cli; hlt", options(nomem, nostack, preserves_flags));
        }
    }
}
