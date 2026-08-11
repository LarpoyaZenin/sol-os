//! Desktop wallpaper, loaded as a Limine boot module (a PNG file).
//!
//! Port of `kernel/wallpaper.c`. At boot, `init()` scans the boot
//! modules for one whose path contains "wallpaper", decodes it with the
//! built-in PNG decoder, and stashes the 0x00RRGGBB bitmap on the heap.
//! `render_region` copies it into the compositor backbuffer, clipped to
//! both the screen and the active damage region.

use crate::boot::requests;
use crate::memory::kheap;

use super::backbuffer;
use super::png;

static mut WALL: *mut u32 = core::ptr::null_mut();
static mut WALL_W: u32 = 0;
static mut WALL_H: u32 = 0;

/// Finds the wallpaper module, decodes it, and returns true on success.
pub fn init() -> bool {
    let mr = unsafe { requests::modules() };
    let Some(mr) = mr else {
        crate::kprintln!("[wallpaper] no modules loaded, using gradient");
        return false;
    };
    if mr.module_count == 0 || mr.modules.is_null() {
        crate::kprintln!("[wallpaper] no modules loaded, using gradient");
        return false;
    }

    let mut found: *mut requests::LimineFile = core::ptr::null_mut();
    let mut found_path = "";
    for i in 0..mr.module_count as usize {
        let file = unsafe { (*mr.modules).add(i).read() };
        let path = if file.is_null() {
            ""
        } else {
            cstr_to_str(unsafe { (*file).path })
        };
        crate::kprintln!("[wallpaper] module {}: '{}'", i, path);
        if path.contains("wallpaper") {
            found = file;
            found_path = path;
            break;
        }
    }

    if found.is_null() {
        crate::kprintln!("[wallpaper] wallpaper module not found, using gradient");
        return false;
    }
    let f = unsafe { &*found };
    if f.address.is_null() || f.size == 0 {
        crate::kprintln!("[wallpaper] wallpaper module not found, using gradient");
        return false;
    }

    crate::kprintln!("[wallpaper] module '{}' {} bytes", found_path, f.size);

    let data = unsafe { core::slice::from_raw_parts(f.address, f.size as usize) };
    let Some((img, w, h)) = png::decode(data) else {
        crate::kprintln!("[wallpaper] PNG decode failed, using gradient");
        return false;
    };

    unsafe {
        WALL = img;
        WALL_W = w;
        WALL_H = h;
    }
    crate::kprintln!(
        "[wallpaper] decoded {}x{}, {} KiB bitmap",
        w,
        h,
        (w as u64 * h as u64 * 4) >> 10
    );
    true
}

pub fn ready() -> bool {
    unsafe { !WALL.is_null() }
}

pub fn width() -> u32 {
    unsafe { WALL_W }
}

pub fn height() -> u32 {
    unsafe { WALL_H }
}

/// Copies `[x0,x1) x [y0,y1)` of the wallpaper into the backbuffer at
/// the same coordinates (the wallpaper is exactly screen-sized).
pub fn render_region(x0: i64, y0: i64, x1: i64, y1: i64) {
    unsafe {
        if WALL.is_null() {
            return;
        }
        let w = WALL_W as i64;
        let h = WALL_H as i64;
        let mut sx0 = x0;
        let mut sy0 = y0;
        let mut sx1 = x1;
        let mut sy1 = y1;
        if sx0 < 0 {
            sx0 = 0;
        }
        if sy0 < 0 {
            sy0 = 0;
        }
        if sx1 > w {
            sx1 = w;
        }
        if sy1 > h {
            sy1 = h;
        }
        if sx1 <= sx0 || sy1 <= sy0 {
            return;
        }
        backbuffer::blit_src(WALL, w, h, sx0, sy0, sx1 - sx0, sy1 - sy0);
    }
}

fn cstr_to_str(p: *mut u8) -> &'static str {
    if p.is_null() {
        return "";
    }
    let mut len = 0usize;
    while unsafe { *p.add(len) } != 0 {
        len += 1;
        if len > 4096 {
            return "";
        }
    }
    let bytes = unsafe { core::slice::from_raw_parts(p as *const u8, len) };
    core::str::from_utf8(bytes).unwrap_or("")
}
