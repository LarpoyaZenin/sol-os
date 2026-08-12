#![no_std]
#![no_main]
#![allow(unsafe_op_in_unsafe_fn)]

mod boot;
mod desktop;
mod drivers;
mod graphics;
mod input;
mod interrupts;
mod kernel;
mod memory;
mod netstack;

core::arch::global_asm!(include_str!("boot/entry.s"));

#[no_mangle]
pub extern "C" fn kernel_main() -> ! {
    kernel::serial::init();
    crate::kprintln!("[rust] sol-os kernel starting");

    let info = boot::init();

    if info.hhdm_offset == 0 {
        crate::kprintln!("[rust] FATAL: no HHDM response from bootloader");
        kernel::halt();
    }
    let hhdm = memory::hhdm::Hhdm::from_offset(info.hhdm_offset);

    let memmap = match unsafe { boot::requests::memmap_response() } {
        Some(m) => m,
        None => {
            crate::kprintln!("[rust] FATAL: no memmap response from bootloader");
            kernel::halt();
        }
    };
    memory::init(&hhdm, memmap);

    let fb = match info.framebuffer {
        Some(fb) => fb,
        None => {
            crate::kprintln!("[rust] FATAL: no framebuffer available");
            kernel::halt();
        }
    };

    crate::kprintln!("[rust] framebuffer: {}x{} bpp={} pitch={}",
        fb.width, fb.height, fb.bpp, fb.pitch);

    interrupts::gdt::init();
    crate::kprintln!("[rust] GDT loaded");

    interrupts::idt::init();
    crate::kprintln!("[rust] IDT loaded");

    interrupts::pic::remap();
    crate::kprintln!("[rust] PIC remapped (IRQs now at vectors 32-47)");

    interrupts::timer::init(100);
    crate::kprintln!("[rust] Timer initialized at 100 Hz");

    let num_devs = drivers::pci::init();
    crate::kprintln!("[rust] PCI: initialization complete ({} device(s))", num_devs);

    unsafe { interrupts::enable() };
    crate::kprintln!("[rust] interrupts enabled");

    drivers::ps2::keyboard_init();
    drivers::ps2::mouse_init();
    drivers::virtio::input::init(&hhdm);

    let net_ok = drivers::virtio::net::init(&hhdm);
    crate::kprintln!("[rust] virtio-net: {}", if net_ok { "up" } else { "not found" });

    crate::kprintln!("[rust] initializing desktop");
    desktop::desktop_init(&fb, hhdm.offset());

    crate::kprintln!("[rust] entering desktop loop");
    loop {
        desktop::desktop_poll(&fb);
        interrupts::idle();
    }
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    crate::kprintln!("[rust] PANIC: {}", info);
    kernel::halt();
}
