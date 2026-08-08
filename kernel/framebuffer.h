#ifndef SOL_FRAMEBUFFER_H
#define SOL_FRAMEBUFFER_H

#include <stdint.h>
#include "limine.h"

/* Sets the active framebuffer from a Limine response. Returns 0 on
 * success, -1 if no framebuffer was provided (e.g. bootloader
 * couldn't set a video mode). */
int fb_init(struct limine_framebuffer *fb);

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
