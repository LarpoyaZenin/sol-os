#ifndef SOL_FRAMEBUFFER_H
#define SOL_FRAMEBUFFER_H

#include <stdint.h>
#include "limine.h"

/* Sets the active framebuffer from a Limine response. Returns 0 on
 * success, -1 if no framebuffer was provided (e.g. bootloader
 * couldn't set a video mode). */
int fb_init(struct limine_framebuffer *fb);

/* ---- page-flipped double buffering ----
 *
 * When enabled, VRAM holds two full-screen pages and every primitive
 * targets the currently "active" page, which is flipped into the
 * scanout with fb_flip_pages(). This lets the desktop composite
 * damage onto the back page and present it atomically (ideally at
 * vblank), so the display never shows a half-updated frame. If the
 * VBE interface or enough VRAM is unavailable, double buffering stays
 * off and the whole stack degrades to the original single-buffer
 * behaviour. `hhdm` is the higher-half direct-map offset (used to
 * recover the framebuffer's physical address so the second VRAM page
 * can be mapped in — the bootloader only maps the visible surface). */
int fb_enable_double_buffer(uint64_t hhdm);
int fb_double_buffered(void);

void fb_set_active_page(unsigned page);
uint8_t *fb_active_base(void);
uint8_t *fb_page_base(unsigned page);

/* Present the active page: updates the VBE scanout offset (no-op when
 * single-buffered). */
void fb_flip_pages(void);

/* Detects whether the VGA vertical-retrace bit actually toggles on
 * this machine/emulator, so callers can wait for vblank without ever
 * hanging on a device that does not drive it. */
void fb_vsync_probe(void);
int  fb_vsync_live(void);
int  fb_vblank_active(void);

void fb_clear(uint32_t color);

/* Core pixel/rect primitives. All coordinates are signed and every
 * operation is clipped to the framebuffer, so callers may pass
 * negative or off-screen coordinates, zero or oversized extents, etc.
 * Without side effects. */
void fb_put_pixel(int64_t x, int64_t y, uint32_t color);
void fb_fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color);

/* Fills the region with `color` (same as fill_rect; named for
 * clarity when restoring background). */
void fb_clear_region(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color);

/* Outlines an axis-aligned rectangle (1px border, clipped). */
void fb_draw_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color);

/* Draws one glyph of the built-in 8x8 bitmap font at (x, y), which
 * is the top-left corner of the glyph cell, using foreground color
 * `fg` and leaving the background untouched (transparent). */
void fb_draw_char(int64_t x, int64_t y, char c, uint32_t fg);

/* Draws a NUL-terminated string left-to-right, 8px per glyph, no
 * wrapping. Returns the x position just past the last glyph drawn. */
int64_t fb_draw_string(int64_t x, int64_t y, const char *s, uint32_t fg);

/* Width in pixels of a string at the 8x8 font size. */
int64_t fb_text_width(const char *s);

#endif /* SOL_FRAMEBUFFER_H */
