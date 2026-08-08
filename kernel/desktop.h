#ifndef SOL_DESKTOP_H
#define SOL_DESKTOP_H

#include <stdint.h>
#include "limine.h"

/* Desktop UI layer rendered on top of the framebuffer: gradient
 * background, mouse cursor, bottom taskbar with a Start button and a
 * live clock/date, and (from the window system step on) windows and
 * desktop icons. All compositing goes through a full-screen
 * backbuffer; only changed regions are blitted to the framebuffer so
 * cursor motion stays cheap. */

/* Builds the desktop scene and shows the cursor. Requires the
 * framebuffer to be initialized (fb_init) and the kernel heap to be
 * up (kheap_init) — call after the PCI/VirtIO setup. `hhdm` is the
 * higher-half direct-map offset, forwarded to the framebuffer layer
 * so it can map the second VRAM page for page flipping. */
void desktop_init(struct limine_framebuffer *fb, uint64_t hhdm);

/* One poll from the main loop: drains VirtIO input, moves the cursor
 * with the VirtIO mouse, dispatches clicks (window management), and
 * refreshes the clock. */
void desktop_poll(void);

#endif /* SOL_DESKTOP_H */
