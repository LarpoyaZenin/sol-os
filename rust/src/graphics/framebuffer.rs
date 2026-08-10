//! Framebuffer access (32bpp, pitch-aware). Mirrors the clipping and
//! addressing rules of `kernel/framebuffer.c`.

#[derive(Clone, Copy)]
pub struct Framebuffer {
    pub address: *mut u8,
    pub width: u64,
    pub height: u64,
    pub pitch: u64,
    pub bpp: u16,
}

impl Framebuffer {
    /// Write one 32-bit pixel, clipping to the visible framebuffer.
    pub fn put_pixel(&self, x: i64, y: i64, color: u32) {
        if x < 0 || y < 0 {
            return;
        }
        if x >= self.width as i64 || y >= self.height as i64 {
            return;
        }
        let bpp = self.bpp as usize / 8;
        let offset = y as usize * self.pitch as usize + x as usize * bpp;
        unsafe {
            let p = self.address.add(offset) as *mut u32;
            *p = color;
        }
    }

    /// Fill a rectangle, clipped to the framebuffer.
    pub fn fill_rect(&self, x: i64, y: i64, w: u64, h: u64, color: u32) {
        for yy in y..(y + h as i64) {
            for xx in x..(x + w as i64) {
                self.put_pixel(xx, yy, color);
            }
        }
    }
}
