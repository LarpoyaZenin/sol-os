//! Boot-time setup: gather Limine responses into a `BootInfo`.

pub mod requests;

use crate::graphics::framebuffer::Framebuffer;

pub struct BootInfo {
    pub framebuffer: Option<Framebuffer>,
    pub hhdm_offset: u64,
    pub memmap_entries: u64,
    pub memmap_usable_bytes: u64,
}

pub fn init() -> BootInfo {
    crate::kprintln!(
        "[boot] limine base revision: {}",
        requests::base_revision()
    );

    let mut info = BootInfo {
        framebuffer: None,
        hhdm_offset: 0,
        memmap_entries: 0,
        memmap_usable_bytes: 0,
    };

    unsafe {
        match requests::framebuffer() {
            Some(fb) => {
                crate::kprintln!(
                    "[boot] framebuffer: {}x{} bpp={} pitch={} @ {:#x}",
                    fb.width,
                    fb.height,
                    fb.bpp,
                    fb.pitch,
                    fb.address as usize
                );
                info.framebuffer = Some(Framebuffer {
                    address: fb.address,
                    width: fb.width,
                    height: fb.height,
                    pitch: fb.pitch,
                    bpp: fb.bpp,
                });
            }
            None => crate::kprintln!("[boot] WARN: no framebuffer response"),
        }

        match requests::hhdm_offset() {
            Some(offset) => {
                crate::kprintln!("[boot] hhdm offset: {:#x}", offset);
                info.hhdm_offset = offset;
            }
            None => crate::kprintln!("[boot] WARN: no hhdm response"),
        }

        match requests::memmap() {
            Some((count, usable)) => {
                crate::kprintln!(
                    "[boot] memmap: {} entries, {} MiB usable",
                    count,
                    usable >> 20
                );
                info.memmap_entries = count;
                info.memmap_usable_bytes = usable;
            }
            None => crate::kprintln!("[boot] WARN: no memmap response"),
        }
    }

    info
}
