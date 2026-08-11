//! Full-screen backbuffer and its drawing primitives.
//!
//! Port of the `bb_*` functions in `kernel/desktop.c`. The scene is
//! composited into a full-screen 32-bpp buffer (one `u32` per pixel)
//! and damage regions are blitted to the framebuffer in `present()`.
//! Every primitive is clipped first to the screen, then to the active
//! clip rectangle set by `scene_region` so nothing is ever painted
//! outside the pixels that will be blitted.

use crate::graphics::framebuffer::Framebuffer;
use crate::memory::kheap;

use super::font::{FONT, FONT_H, FONT_W};

/* Palette for the vertical background gradient. */
const BG_TOP: u32 = 0x003A5C8A;
const BG_BOTTOM: u32 = 0x00121B2E;

/* Backbuffer guard: the allocation is bb_bytes + 64 and the trailing
 * words are stamped with canaries. Any out-of-bounds backbuffer write
 * clobbers a canary and is detected by `desktop_gfx_integrity`. */
const BB_GUARD_BYTES: u64 = 64;
const BB_CANARIES: usize = 16;
const BB_CANARY_BASE: u32 = 0xC0FFEE00;

static mut BB: *mut u32 = core::ptr::null_mut();
static mut SCREEN_W: i64 = 0;
static mut SCREEN_H: i64 = 0;

/* Active clip rectangle for backbuffer writes. */
static mut CLIP_ACTIVE: bool = false;
static mut CLIP_X0: i64 = 0;
static mut CLIP_Y0: i64 = 0;
static mut CLIP_X1: i64 = 0;
static mut CLIP_Y1: i64 = 0;

pub fn screen_w() -> i64 {
    unsafe { SCREEN_W }
}

pub fn screen_h() -> i64 {
    unsafe { SCREEN_H }
}

pub fn pixel(x: i64, y: i64) -> u32 {
    if x < 0 || y < 0 || x >= unsafe { SCREEN_W } || y >= unsafe { SCREEN_H } {
        return 0;
    }
    unsafe { *BB.offset((y as usize * SCREEN_W as usize + x as usize) as isize) }
}

/// Allocates the backbuffer (plus a guard region) and stamps the
/// canaries. Returns false on allocation failure.
pub fn init(w: i64, h: i64) -> bool {
    let bytes = (w as u64) * (h as u64) * 4;
    let ptr = kheap::kmalloc(bytes + BB_GUARD_BYTES);
    if ptr.is_null() {
        return false;
    }
    unsafe {
        BB = ptr as *mut u32;
        SCREEN_W = w;
        SCREEN_H = h;
        CLIP_ACTIVE = false;
        for i in 0..BB_CANARIES {
            let want = BB_CANARY_BASE.wrapping_add(i as u32);
            *BB.offset((w as usize * h as usize + i) as isize) = want;
        }
    }
    true
}

/// Returns 0 when all guard canaries are intact.
pub fn canary_corruption() -> u32 {
    unsafe {
        for i in 0..BB_CANARIES {
            let want = BB_CANARY_BASE.wrapping_add(i as u32);
            let got = *BB.offset((SCREEN_W as usize * SCREEN_H as usize + i) as isize);
            if got != want {
                return got;
            }
        }
    }
    0
}

/* ---- clipping ---- */

#[inline]
fn clip_region_impl(x: i64, y: i64, w: i64, h: i64) -> Option<(i64, i64, i64, i64)> {
    if w <= 0 || h <= 0 {
        return None;
    }

    let g_w = unsafe { SCREEN_W };
    let g_h = unsafe { SCREEN_H };

    /* Left edge: clip negative to 0. */
    let left = if x < 0 { 0 } else { x };
    if left >= g_w {
        return None;
    }

    /* Right edge computed from the UNCLIPPED x so negative origins
     * shrink the extent correctly, and saturated so x + w can never
     * overflow. */
    let right = if x >= 0 {
        if w >= g_w - x {
            g_w
        } else {
            x + w
        }
    } else {
        let r = x + w;
        if r > g_w {
            g_w
        } else {
            r
        }
    };
    if right <= left {
        return None;
    }

    let top = if y < 0 { 0 } else { y };
    if top >= g_h {
        return None;
    }

    let bottom = if y >= 0 {
        if h >= g_h - y {
            g_h
        } else {
            y + h
        }
    } else {
        let b = y + h;
        if b > g_h {
            g_h
        } else {
            b
        }
    };
    if bottom <= top {
        return None;
    }

    let mut x0 = left;
    let mut y0 = top;
    let mut x1 = right;
    let mut y1 = bottom;

    unsafe {
        if CLIP_ACTIVE {
            if x0 < CLIP_X0 {
                x0 = CLIP_X0;
            }
            if y0 < CLIP_Y0 {
                y0 = CLIP_Y0;
            }
            if x1 > CLIP_X1 {
                x1 = CLIP_X1;
            }
            if y1 > CLIP_Y1 {
                y1 = CLIP_Y1;
            }
            if x1 <= x0 || y1 <= y0 {
                return None;
            }
        }
    }
    Some((x0, y0, x1, y1))
}

/// Scopes every subsequent backbuffer write to `[x0,x1) x [y0,y1)`.
pub fn clip_set(x: i64, y: i64, w: i64, h: i64) {
    unsafe {
        CLIP_ACTIVE = true;
        CLIP_X0 = x;
        CLIP_Y0 = y;
        CLIP_X1 = x + w;
        CLIP_Y1 = y + h;
    }
}

/// Returns the current active clip rectangle as exclusive bounds.
pub fn clip_bounds() -> (i64, i64, i64, i64) {
    unsafe { (CLIP_X0, CLIP_Y0, CLIP_X1, CLIP_Y1) }
}

/// Restores a previously saved clip state (used by `scene_region`).
pub fn restore_clip(active: bool, x0: i64, y0: i64, x1: i64, y1: i64) {
    unsafe {
        CLIP_ACTIVE = active;
        CLIP_X0 = x0;
        CLIP_Y0 = y0;
        CLIP_X1 = x1;
        CLIP_Y1 = y1;
    }
}

/// Clips `[x,y) x [x+w,y+h)` to the backbuffer bounds (and to the
/// active clip rect when one is set), returning the exclusive bounds.
pub fn clip_region(x: i64, y: i64, w: i64, h: i64) -> Option<(i64, i64, i64, i64)> {
    clip_region_impl(x, y, w, h)
}

/// Remembers and clears the active clip rectangle (used around the
/// cursor stamp, which floats above the scene).
pub fn clip_save_clear() {
    unsafe {
        CLIP_ACTIVE = false;
    }
}

pub fn clip_restore() {
    unsafe {
        CLIP_ACTIVE = true;
    }
}

pub fn clip_clear() {
    unsafe {
        CLIP_ACTIVE = false;
    }
}

pub fn clip_active() -> bool {
    unsafe { CLIP_ACTIVE }
}

/* ---- primitives ---- */

pub fn put_pixel(x: i64, y: i64, c: u32) {
    unsafe {
        if x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H {
            return;
        }
        if CLIP_ACTIVE && (x < CLIP_X0 || x >= CLIP_X1 || y < CLIP_Y0 || y >= CLIP_Y1) {
            return;
        }
        *BB.offset((y as usize * SCREEN_W as usize + x as usize) as isize) = c;
    }
}

pub fn fill_rect(x: i64, y: i64, w: i64, h: i64, c: u32) {
    if let Some((x0, y0, x1, y1)) = clip_region(x, y, w, h) {
        unsafe {
            let g_w = SCREEN_W;
            for yy in y0..y1 {
                let row = BB.offset((yy as usize * g_w as usize) as isize);
                for xx in x0..x1 {
                    *row.offset(xx as isize) = c;
                }
            }
        }
    }
}

pub fn draw_rect(x: i64, y: i64, w: i64, h: i64, c: u32) {
    if w <= 0 || h <= 0 {
        return;
    }
    fill_rect(x, y, w, 1, c);
    fill_rect(x, y + h - 1, w, 1, c);
    fill_rect(x, y, 1, h, c);
    fill_rect(x + w - 1, y, 1, h, c);
}

pub fn clear_region(x: i64, y: i64, w: i64, h: i64, c: u32) {
    fill_rect(x, y, w, h, c);
}

/// Draws one 8x8 glyph; each set bit paints one pixel (LSB first).
pub fn draw_char(x: i64, y: i64, ch: u8, fg: u32) {
    let glyph = FONT[ch as usize];
    for row in 0..FONT_H {
        let bits = glyph[row as usize];
        if bits == 0 {
            continue;
        }
        for col in 0..FONT_W {
            if (bits >> col) & 1 != 0 {
                put_pixel(x + col, y + row, fg);
            }
        }
    }
}

/// Draws a NUL-terminated byte string, advancing one glyph width per
/// character.
pub fn draw_string(x: i64, y: i64, s: &[u8], fg: u32) {
    let mut xx = x;
    for &ch in s {
        if ch == 0 {
            break;
        }
        draw_char(xx, y, ch, fg);
        xx += FONT_W;
    }
}

/// Draws a string with each glyph scaled up by `scale` (2 = double
/// size), used for the browser wordmark and the IST clock.
pub fn draw_string_scaled(x: i64, y: i64, s: &[u8], fg: u32, scale: i64) {
    let scale = if scale < 1 { 1 } else { scale };
    let mut xx = x;
    for &ch in s {
        if ch == 0 {
            break;
        }
        let glyph = FONT[ch as usize];
        for r in 0..FONT_H {
            let bits = glyph[r as usize];
            if bits == 0 {
                continue;
            }
            for c in 0..FONT_W {
                if (bits >> c) & 1 != 0 {
                    fill_rect(xx + c * scale, y + r * scale, scale, scale, fg);
                }
            }
        }
        xx += FONT_W * scale;
    }
}

/// Vertical gradient color at row `y` of an `h`-tall screen.
pub fn gradient_color(y: i64, h: i64) -> u32 {
    let r0 = (BG_TOP >> 16) & 0xFF;
    let g0 = (BG_TOP >> 8) & 0xFF;
    let b0 = BG_TOP & 0xFF;
    let r1 = (BG_BOTTOM >> 16) & 0xFF;
    let g1 = (BG_BOTTOM >> 8) & 0xFF;
    let b1 = BG_BOTTOM & 0xFF;
    let t = if h <= 1 {
        0u32
    } else {
        ((y as u64) * 255u64 / ((h - 1) as u64)) as u32
    };
    let r = r0 + (r1 - r0) * t / 255;
    let g = g0 + (g1 - g0) * t / 255;
    let b = b0 + (b1 - b0) * t / 255;
    (r << 16) | (g << 8) | b
}

/// Fills `[x0,x1) x [y0,y1)` with the per-row gradient.
pub fn fill_rect_gradient(x0: i64, y0: i64, x1: i64, y1: i64) {
    unsafe {
        let g_w = SCREEN_W;
        let g_h = SCREEN_H;
        for yy in y0..y1 {
            let c = gradient_color(yy, g_h);
            let row = BB.offset((yy as usize * g_w as usize) as isize);
            for xx in x0..x1 {
                *row.offset(xx as isize) = c;
            }
        }
    }
}

/* ---- blits (backbuffer -> framebuffer) ---- */

fn blit_copy_u32(dst: *mut u32, src: *const u32, n: usize) {
    unsafe {
        for i in 0..n {
            *dst.add(i) = *src.add(i);
        }
    }
}

/// Copies the damage rect from the backbuffer to the framebuffer.
pub fn blit_rect(x: i64, y: i64, w: i64, h: i64, fb: &Framebuffer) {
    if let Some((x0, y0, x1, y1)) = clip_region(x, y, w, h) {
        unsafe {
            let g_w = SCREEN_W;
            let pitch = fb.pitch as i64;
            let base = fb.address;
            for yy in y0..y1 {
                let dst = base.add((yy as usize * pitch as usize + x0 as usize * 4) as usize)
                    as *mut u32;
                let src = BB.offset((yy as usize * g_w as usize + x0 as usize) as isize);
                blit_copy_u32(dst, src, (x1 - x0) as usize);
            }
        }
    }
}

pub fn blit_full(fb: &Framebuffer) {
    blit_rect(0, 0, unsafe { SCREEN_W }, unsafe { SCREEN_H }, fb);
}

/// Copies a `dw x dh` rectangle of a `sw`-wide row-major `u32` image
/// (`src`, 0x00RRGGBB) into the backbuffer at `(dx0, dy0)`. Both the
/// source and destination are clipped to the active clip rect / screen
/// bounds; source pixels outside `[0,sw)` are skipped (black padding).
pub fn blit_src(src: *const u32, sw: i64, sh: i64, dx0: i64, dy0: i64, dw: i64, dh: i64) {
    if let Some((x0, y0, x1, y1)) = clip_region(dx0, dy0, dw, dh) {
        unsafe {
            let g_w = SCREEN_W;
            for yy in y0..y1 {
                let sy = yy - dy0;
                if sy < 0 || sy >= sh {
                    continue;
                }
                let src_row = src.add((sy as usize * sw as usize) as isize);
                let dst_row = BB.offset((yy as usize * g_w as usize) as isize);
                for xx in x0..x1 {
                    let sx = xx - dx0;
                    if sx < 0 || sx >= sw {
                        continue;
                    }
                    *dst_row.add(xx as isize) = *src_row.add(sx as isize);
                }
            }
        }
    }
}
