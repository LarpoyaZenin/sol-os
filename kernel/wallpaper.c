#include "wallpaper.h"
#include "png.h"
#include "klog.h"
#include "limine_requests.h"
#include "mm/kheap.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

static uint32_t *g_wall = NULL;   /* 0x00RRGGBB, row-major */
static uint32_t  g_wall_w = 0;
static uint32_t  g_wall_h = 0;

static const char *module_path(const struct limine_file *f) {
    return f->path ? f->path : "";
}

/* True when `path` contains the substring `needle`. */
static int path_contains(const char *path, const char *needle) {
    for (size_t i = 0; path[i]; i++) {
        size_t j = 0;
        while (needle[j] && path[i + j] && path[i + j] == needle[j]) j++;
        if (needle[j] == 0) return 1;
    }
    return 0;
}

int wallpaper_init(void) {
    const struct limine_module_response *mr = module_request.response;
    if (mr == NULL || mr->module_count == 0) {
        klog("[wallpaper] no modules loaded, using gradient\n");
        return 0;
    }

    const struct limine_file *file = NULL;
    for (uint64_t i = 0; i < mr->module_count; i++) {
        const char *path = module_path(mr->modules[i]);
        klog("[wallpaper] module %lu: '%s'\n",
             (unsigned long)i, path);
        if (path_contains(path, "wallpaper")) {
            file = mr->modules[i];
            break;
        }
    }
    if (file == NULL || file->address == NULL || file->size == 0) {
        klog("[wallpaper] wallpaper module not found, using gradient\n");
        return 0;
    }

    klog("[wallpaper] module '%s' %lu bytes\n",
         module_path(file), (unsigned long)file->size);

    uint32_t *img = NULL;
    uint32_t w = 0, h = 0;
    if (!png_decode(file->address, (size_t)file->size, &img, &w, &h)) {
        klog("[wallpaper] PNG decode failed, using gradient\n");
        return 0;
    }

    g_wall = img;
    g_wall_w = w;
    g_wall_h = h;
    klog("[wallpaper] decoded %ux%u, %lu KiB bitmap\n",
         (unsigned)w, (unsigned)h,
         (unsigned long)((uint64_t)w * h * 4u / 1024u));
    return 1;
}

int wallpaper_ready(void) {
    return g_wall != NULL;
}

uint32_t wallpaper_width(void) {
    return g_wall_w;
}

uint32_t wallpaper_height(void) {
    return g_wall_h;
}

void wallpaper_render(uint32_t *bb, uint64_t bb_w,
                      int64_t x0, int64_t y0, int64_t x1, int64_t y1) {
    if (g_wall == NULL) return;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)g_wall_w) x1 = (int64_t)g_wall_w;
    if (y1 > (int64_t)g_wall_h) y1 = (int64_t)g_wall_h;
    if (x1 <= x0 || y1 <= y0) return;

    uint64_t w = (uint64_t)(x1 - x0);
    for (int64_t yy = y0; yy < y1; yy++) {
        const uint32_t *src = g_wall + (uint64_t)yy * g_wall_w + (uint64_t)x0;
        uint32_t *dst = bb + (uint64_t)yy * bb_w + (uint64_t)x0;
        for (uint64_t xx = 0; xx < w; xx++) dst[xx] = src[xx];
    }
}
