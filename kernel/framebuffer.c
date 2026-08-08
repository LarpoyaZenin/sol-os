#include "framebuffer.h"
#include "font.h"
#include "klog.h"
#include <stddef.h>
#include <stdint.h>

/* Framebuffer driver.
 *
 * The active framebuffer is described by a limine_framebuffer: base
 * address, width, height, bits-per-pixel and *pitch* (bytes per row).
 * Pitch may be larger than width * bpp (padding) and MUST always be
 * used when addressing rows; it is never assumed to equal width * 4.
 *
 * All primitives take signed coordinates and clip to the framebuffer
 * before writing a single pixel. They handle negative/off-screen
 * coordinates, zero-sized and oversized rectangles, and overflow-free
 * extent arithmetic (extents are saturated to the framebuffer bounds
 * instead of being added up). No primitive ever writes outside
 * [0,width) x [0,height). */

static struct limine_framebuffer *g_fb = NULL;
static uint64_t g_w = 0, g_h = 0;      /* framebuffer dimensions */
static uint8_t *g_fbaddr = NULL;
static uint32_t g_bpp = 4u;            /* bytes per pixel (clamped to 32bpp) */

int fb_init(struct limine_framebuffer *fb) {
    if (fb == NULL) return -1;
    g_fb = fb;
    g_fbaddr = (uint8_t *)fb->address;
    g_w = fb->width;
    g_h = fb->height;
    g_bpp = fb->bpp / 8u;
    if (g_bpp == 0u) g_bpp = 4u;
    if (g_bpp != 4u) {
        /* The whole graphics stack writes 32-bit pixels; other depths
         * are not supported. Keep going (pitch-addressed) but warn. */
        klog("[fb] WARNING: unsupported bpp %u (assuming 32 bpp)\n",
             (unsigned)fb->bpp);
        g_bpp = 4u;
    }
    return 0;
}

/* Clips a (possibly negative/oversized) rect to the framebuffer.
 * Returns 1 and fills *x0..*y1 (0-based, exclusive) when a non-empty
 * intersection exists, else 0. Overflow-safe: extents are saturated
 * to the framebuffer bounds rather than computed as x + w. */
static int fb_clip(int64_t x, int64_t y, int64_t w, int64_t h,
                   uint64_t *x0, uint64_t *y0, uint64_t *x1, uint64_t *y1) {
    if (w <= 0 || h <= 0) return 0;

    int64_t left = x < 0 ? 0 : x;
    if (left >= (int64_t)g_w) return 0;

    int64_t right;
    if (x >= 0) {
        right = (w >= (int64_t)g_w - x) ? (int64_t)g_w : x + w;
    } else {
        right = x + w;
        if (right > (int64_t)g_w) right = (int64_t)g_w;
    }
    if (right <= left) return 0;

    int64_t top = y < 0 ? 0 : y;
    if (top >= (int64_t)g_h) return 0;

    int64_t bottom;
    if (y >= 0) {
        bottom = (h >= (int64_t)g_h - y) ? (int64_t)g_h : y + h;
    } else {
        bottom = y + h;
        if (bottom > (int64_t)g_h) bottom = (int64_t)g_h;
    }
    if (bottom <= top) return 0;

    *x0 = (uint64_t)left;
    *y0 = (uint64_t)top;
    *x1 = (uint64_t)right;
    *y1 = (uint64_t)bottom;
    return 1;
}

void fb_put_pixel(int64_t x, int64_t y, uint32_t color) {
    if (x < 0 || y < 0 || (uint64_t)x >= g_w || (uint64_t)y >= g_h) return;
    uint8_t *base = g_fbaddr;
    uint32_t *pixel = (uint32_t *)(base + (uint64_t)y * g_fb->pitch +
                                   (uint64_t)x * g_bpp);
    *pixel = color;
}

void fb_fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color) {
    uint64_t x0, y0, x1, y1;
    if (!fb_clip(x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (uint64_t yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)(g_fbaddr + yy * g_fb->pitch + x0 * g_bpp);
        for (uint64_t xx = x0; xx < x1; xx++) row[xx - x0] = color;
    }
}

void fb_clear_region(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color);
}

void fb_draw_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    fb_fill_rect(x, y, w, 1, color);
    fb_fill_rect(x, y + h - 1, w, 1, color);
    fb_fill_rect(x, y, 1, h, color);
    fb_fill_rect(x + w - 1, y, 1, h, color);
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, (int64_t)g_w, (int64_t)g_h, color);
}

void fb_draw_char(int64_t x, int64_t y, char c, uint32_t fg) {
    unsigned char idx = (unsigned char)c;
    if (idx > 127) return;

    const uint8_t *glyph = font8x8_basic[idx];
    for (uint64_t row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (uint64_t col = 0; col < FONT_W; col++) {
            if ((bits >> col) & 1) {
                fb_put_pixel(x + (int64_t)col, y + (int64_t)row, fg);
            }
        }
    }
}

int64_t fb_draw_string(int64_t x, int64_t y, const char *s, uint32_t fg) {
    if (s == NULL) return x;
    while (*s) {
        fb_draw_char(x, y, *s++, fg);
        x += FONT_W;
    }
    return x;
}

int64_t fb_text_width(const char *s) {
    int64_t len = 0;
    if (s == NULL) return 0;
    while (s[len]) len++;
    return len * FONT_W;
}
