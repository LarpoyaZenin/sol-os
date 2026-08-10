//! Graphics: framebuffer and the boot test pattern.

pub mod framebuffer;

use framebuffer::Framebuffer;

/// Draw a full-screen vertical 7-band rainbow with a white 1px border.
/// Milestone-1 proof that we can address the Limine framebuffer.
pub fn test_pattern(fb: &Framebuffer) {
    let bands: [u32; 7] = [
        0x00FF0000, // red
        0x00FF8000, // orange
        0x00FFFF00, // yellow
        0x0000FF00, // green
        0x000000FF, // blue
        0x004B0082, // indigo
        0x00EE82EE, // violet
    ];

    let n = bands.len() as i64;
    let band_w = fb.width as i64 / n;
    for (i, &color) in bands.iter().enumerate() {
        fb.fill_rect(i as i64 * band_w, 0, band_w as u64, fb.height, color);
    }

    // 1px white border to verify edge addressing.
    let w = fb.width as i64;
    let h = fb.height as i64;
    fb.fill_rect(0, 0, w as u64, 1, 0x00FFFFFF);
    fb.fill_rect(0, h - 1, w as u64, 1, 0x00FFFFFF);
    fb.fill_rect(0, 0, 1, h as u64, 0x00FFFFFF);
    fb.fill_rect(w - 1, 0, 1, h as u64, 0x00FFFFFF);
}
