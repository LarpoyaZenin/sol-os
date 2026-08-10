#![no_std]
#![no_main]
#![allow(unsafe_op_in_unsafe_fn)]

mod boot;
mod drivers;
mod graphics;
mod input;
mod interrupts;
mod kernel;
mod memory;

core::arch::global_asm!(include_str!("boot/entry.s"));

/// Entry point called by `boot/entry.s`. Never returns.
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

    graphics::test_pattern(&fb);

    crate::kprintln!("[rust] framebuffer: {}x{} bpp={} pitch={}",
        fb.width, fb.height, fb.bpp, fb.pitch);
    crate::kprintln!("[rust] gfx test pattern drawn");

    /* ---- Milestone 3: interrupt stack ---- */
    interrupts::gdt::init();
    crate::kprintln!("[rust] GDT loaded");

    interrupts::idt::init();
    crate::kprintln!("[rust] IDT loaded");

    interrupts::pic::remap();
    crate::kprintln!("[rust] PIC remapped (IRQs now at vectors 32-47)");

    interrupts::timer::init(100);
    crate::kprintln!("[rust] Timer initialized at 100 Hz");

    /* ---- Milestone 4: centralized input subsystem ---- */
    let num_devs = drivers::pci::init();
    crate::kprintln!("[rust] PCI: initialization complete ({} device(s))", num_devs);

    unsafe { interrupts::enable() };
    crate::kprintln!("[rust] interrupts enabled");

    drivers::ps2::keyboard_init();
    drivers::ps2::mouse_init();
    drivers::virtio::input::init(&hhdm);

    /* Idle loop: drain and log input events, sleep until the PIT wakes
     * us, and log a heartbeat every 500 ticks (~5 s) to prove the
     * timer, keyboard, and mice are all still alive. */
    crate::kprintln!("[rust] entering idle loop");
    let mut last_heartbeat: u64 = 0;
    loop {
        while let Some(ev) = input::pop() {
            log_input(&ev);
        }
        let t = interrupts::timer::ticks();
        if t - last_heartbeat >= 500 {
            crate::kprintln!(
                "[rust][heartbeat] ticks={} key_events={} mouse_events={} dropped={} virtio_devs={} virtio_irq={} virtio_key={} virtio_events={} virtio_dropped={}",
                t,
                input::key_events(),
                input::mouse_events(),
                input::dropped_events(),
                drivers::virtio::input::device_count(),
                drivers::virtio::input::irq_count(),
                drivers::virtio::input::key_count(),
                drivers::virtio::input::event_count(),
                drivers::virtio::input::dropped_count(),
            );
            last_heartbeat = t;
        }
        interrupts::idle();
    }
}

/* Debug consumer: the serial input test/demo mode. Every logical event
 * is logged with enough state to prove modifier handling (shift/caps)
 * and both mouse transport types. */
fn log_input(ev: &input::Event) {
    match ev {
        input::Event::Key {
            code, key, pressed, ch,
        } => {
            let state = format_args!(
                "shift={} caps={}",
                input::shift_pressed(),
                input::caps_lock()
            );
            if *pressed {
                match ch {
                    Some(c) => crate::kprintln!(
                        "[input] keydown code={} key={:?} ch='{}' {}",
                        code,
                        key,
                        c,
                        state
                    ),
                    None => crate::kprintln!(
                        "[input] keydown code={} key={:?} ch=none {}",
                        code,
                        key,
                        state
                    ),
                }
            } else {
                crate::kprintln!(
                    "[input] keyup code={} key={:?} ch=none {}",
                    code,
                    key,
                    state
                );
            }
        }
        input::Event::MouseMove { dx, dy } => {
            crate::kprintln!("[input] mouse x={} y={}", dx, dy);
        }
        input::Event::MouseButton { button, pressed } => {
            crate::kprintln!(
                "[input] button {} {}",
                button.name(),
                if *pressed { "down" } else { "up" }
            );
        }
        input::Event::MouseScroll { delta } => {
            crate::kprintln!("[input] wheel delta={}", delta);
        }
    }
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    crate::kprintln!("[rust] PANIC: {}", info);
    kernel::halt();
}
