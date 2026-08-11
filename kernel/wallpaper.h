#ifndef SOL_WALLPAPER_H
#define SOL_WALLPAPER_H

#include <stdint.h>

/* Desktop wallpaper. The PNG shipped in the ISO (as a Limine module,
 * see boot/limine.conf) is decoded once at boot into a full-screen
 * 0x00RRGGBB bitmap; the desktop compositor copies regions out of it
 * whenever it needs to repaint the background. If the module is
 * missing or unreadable the desktop falls back to its gradient. */

/* Locates the wallpaper module by path, decodes it with the built-in
 * PNG decoder and keeps the bitmap for the life of the session.
 * Returns 1 when the wallpaper is ready, 0 on any failure. */
int wallpaper_init(void);

/* 1 once a decoded wallpaper is available. */
int wallpaper_ready(void);

/* Dimensions of the decoded wallpaper (normally 1920x1080). */
uint32_t wallpaper_width(void);
uint32_t wallpaper_height(void);

/* Copies the wallpaper pixels covering the region [x0,x1)x[y0,y1) of
 * the 32bpp backbuffer `bb` (pitch in pixels `bb_w`). Clipped to the
 * wallpaper bounds, so callers may pass any region. */
void wallpaper_render(uint32_t *bb, uint64_t bb_w,
                      int64_t x0, int64_t y0, int64_t x1, int64_t y1);

#endif /* SOL_WALLPAPER_H */
