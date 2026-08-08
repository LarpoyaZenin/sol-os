#include "framebuffer.h"
#include "font.h"
#include "klog.h"
#include "drivers/vga/vbe.h"
#include "arch/x86_64/paging.h"
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

/* Page-flipped double buffering. Page 0 is the base framebuffer that
 * the bootloader set up; page 1 (if enabled) sits page_bytes further
 * into VRAM and is selected for scanout by a VBE Y-offset write. All
 * pixel primitives target g_page_base[g_active_page]. */
static uint8_t *g_page_base[2] = { NULL, NULL };
static unsigned g_active_page = 0;
static int      g_double = 0;
static uint64_t g_page_bytes = 0;

static int g_vsync_live = 0;

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
    g_page_base[0] = g_fbaddr;
    g_page_base[1] = NULL;
    g_active_page = 0;
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
    uint8_t *base = g_page_base[g_active_page];
    uint32_t *pixel = (uint32_t *)(base + (uint64_t)y * g_fb->pitch +
                                   (uint64_t)x * g_bpp);
    *pixel = color;
}

void fb_fill_rect(int64_t x, int64_t y, int64_t w, int64_t h, uint32_t color) {
    uint64_t x0, y0, x1, y1;
    if (!fb_clip(x, y, w, h, &x0, &y0, &x1, &y1)) return;
    uint8_t *base = g_page_base[g_active_page];
    for (uint64_t yy = y0; yy < y1; yy++) {
        uint32_t *row = (uint32_t *)(base + yy * g_fb->pitch + x0 * g_bpp);
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

/* ---- page-flipped double buffering ---- */

int fb_double_buffered(void) {
    return g_double;
}

uint8_t *fb_active_base(void) {
    return g_page_base[g_active_page];
}

uint8_t *fb_page_base(unsigned page) {
    return g_page_base[page & 1u];
}

void fb_set_active_page(unsigned page) {
    g_active_page = page & 1u;
}

void fb_flip_pages(void) {
    if (!g_double) return;
    uint32_t y_offs = g_active_page * (uint32_t)g_h;
    if (y_offs > 0xFFFFu) return;
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)y_offs);
}

int fb_vblank_active(void) {
    return vga_vblank_active();
}

int fb_vsync_live(void) {
    return g_vsync_live;
}

void fb_vsync_probe(void) {
    int saw0 = 0, saw1 = 0;
    for (unsigned i = 0; i < 20000u; i++) {
        if (fb_vblank_active()) saw1 = 1;
        else saw0 = 1;
        if (saw0 && saw1) break;
    }
    g_vsync_live = saw0 && saw1;
    klog("[fb] vsync %s\n", g_vsync_live ? "live" : "unavailable");
}

/* Probes the Bochs VBE interface and, when it is present and VRAM is
 * large enough, arms two full-screen pages plus a VBE scanout offset
 * for tear-free page flipping. Returns 1 when double buffering is
 * active, 0 (single-buffer fallback) otherwise. */
int fb_enable_double_buffer(uint64_t hhdm) {
    if (g_double) return 1;
    if (g_fb == NULL || g_fbaddr == NULL) {
        klog("[fb] double buffer: no framebuffer\n");
        return 0;
    }

    if (!vbe_present()) {
        klog("[fb] double buffer: no Bochs VBE interface, single-buffered\n");
        return 0;
    }

    uint16_t id    = vbe_read(VBE_DISPI_INDEX_ID);
    uint16_t xres  = vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t yres  = vbe_read(VBE_DISPI_INDEX_YRES);
    uint16_t bpp   = vbe_read(VBE_DISPI_INDEX_BPP);
    uint16_t en    = vbe_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t virtw = vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t virth = vbe_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t mem64 = vbe_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K);

    klog("[vbe] id=%x %ux%u %ubpp enable=%x virt=%ux%u vram=%u KiB\n",
         (unsigned)id, (unsigned)xres, (unsigned)yres, (unsigned)bpp,
         (unsigned)en, (unsigned)virtw, (unsigned)virth,
         (unsigned)mem64 * 64u);

    if (bpp != 32u) {
        klog("[vbe] double buffer: %u bpp unsupported\n", (unsigned)bpp);
        return 0;
    }
    if (!(en & VBE_DISPI_ENABLED)) {
        klog("[vbe] double buffer: display not enabled\n");
        return 0;
    }
    if (!(en & VBE_DISPI_LFB_ENABLED)) {
        klog("[vbe] double buffer: linear framebuffer not enabled\n");
        return 0;
    }
    if (g_w != (uint64_t)xres || g_h != (uint64_t)yres) {
        klog("[vbe] double buffer: mode mismatch (%lux%lu vs %ux%u)\n",
             (unsigned long)g_w, (unsigned long)g_h,
             (unsigned)xres, (unsigned)yres);
        return 0;
    }
    if (g_fb->pitch != (uint64_t)xres * 4u) {
        klog("[vbe] double buffer: pitch %u != stride %u\n",
             (unsigned)g_fb->pitch, (unsigned)xres * 4u);
        return 0;
    }

    uint64_t vram  = (uint64_t)mem64 * 65536u;
    uint64_t page  = (uint64_t)g_fb->pitch * g_h;
    if (page == 0 || page > vram / 2u) {
        klog("[vbe] double buffer: need %lu bytes/page, have %lu\n",
             (unsigned long)page, (unsigned long)vram);
        return 0;
    }

    /* Select the second page with the VBE scanout offset. The write
     * itself is the capability test: some implementations clamp the
     * offset when the target page does not fit in video memory, so if
     * it reads back unchanged, both pages are reachable. (QEMU's
     * VIRT_HEIGHT register is read-only - it reports how many lines
     * fit in VRAM - so it cannot be used to size the virtual screen;
     * the offset clamp is the real limit.) */
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)yres);
    if (vbe_read(VBE_DISPI_INDEX_Y_OFFSET) != yres) {
        klog("[vbe] double buffer: Y-offset %u rejected\n", (unsigned)yres);
        return 0;
    }
    vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);

    /* The bootloader only mapped the visible surface (pitch*height);
     * the second scanout page lives further into VRAM and has to be
     * mapped by hand. Recover the framebuffer's physical base from the
     * HHDM-mapped address the protocol handed us. Requiring a 4K-aligned
     * page keeps this mapping on fresh page-table entries so it never
     * aliases the bootloader's write-combining framebuffer mapping. */
    if ((page & 0xFFFu) != 0u) {
        klog("[vbe] double buffer: page %lu not 4K-aligned\n",
             (unsigned long)page);
        return 0;
    }
    uint64_t fb_phys = (uint64_t)g_fbaddr - hhdm;
    paging_map_physical(hhdm, fb_phys + page, page);

    /* Write/readback probe: proves the CPU can actually reach page 2
     * through the mapping we just installed, not just that the scanout
     * register accepted the offset. */
    uint8_t *probe = g_fbaddr + page;
    const uint32_t w0 = 0xCAFEBABEu, w1 = 0xDEADBEEFu;
    *(volatile uint32_t *)probe = w0;
    *(volatile uint32_t *)(probe + page - 4u) = w1;
    if (*(volatile uint32_t *)probe != w0 ||
        *(volatile uint32_t *)(probe + page - 4u) != w1) {
        klog("[vbe] double buffer: page 2 readback mismatch\n");
        return 0;
    }

    g_page_bytes = page;
    g_page_base[1] = probe;
    g_double = 1;

    klog("[vbe] double buffering ENABLED: 2 x %lu bytes pages in %lu bytes "
         "VRAM, scanout via Y-offset flips\n",
         (unsigned long)g_page_bytes, (unsigned long)vram);
    return 1;
}
