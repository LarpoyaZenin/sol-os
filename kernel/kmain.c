#include "klog.h"
#include "framebuffer.h"
#include "font.h"
#include "desktop.h"
#include "limine_requests.h"
#include "mm/pmm.h"
#include "mm/kheap.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/timer.h"
#include "arch/x86_64/keyboard.h"
#include "arch/x86_64/mouse.h"
#include "drivers/pci/pci.h"
#include "drivers/virtio/virtio_input.h"
#include "string.h"
#include <stddef.h>
#include <stdint.h>

/* Phase 1 + Phase 2 + desktop milestone: boot to QEMU, framebuffer
 * with on-screen text, serial logging, own GDT/IDT, PIC remap, PIT
 * timer, PS/2 keyboard, PS/2 mouse, a physical page manager, a kernel
 * heap, VirtIO keyboard/mouse input, and a desktop UI (background,
 * cursor, taskbar, clock). This function intentionally never
 * returns. */

static void pmm_selftest(void) {
    uintptr_t p1 = pmm_alloc_page();
    uintptr_t p2 = pmm_alloc_page();
    uintptr_t p3 = pmm_alloc_page();
    klog("PMM selftest: pages %lx %lx %lx\n",
         (unsigned long)p1, (unsigned long)p2, (unsigned long)p3);

    pmm_free_page(p2);
    uintptr_t p4 = pmm_alloc_page();
    klog("PMM selftest: freed p2, realloc got %lx (%s)\n",
         (unsigned long)p4, p4 == p2 ? "OK, reuse" : "not reused");
    pmm_free_page(p1);
    pmm_free_page(p3);
    pmm_free_page(p4);
}

static void heap_selftest(void) {
    void *a = kmalloc(64);
    void *b = kmalloc(512);
    klog("Heap selftest: a=%p b=%p\n", a, b);

    if (a != NULL) memset(a, 0xAB, 64);
    if (b != NULL) memset(b, 0xCD, 512);

    kfree(a);
    void *c = kmalloc(64);   /* should reuse a's block */
    klog("Heap selftest: realloc after free -> %p (%s)\n",
         c, c == a ? "OK, reused" : "different block");

    kfree(b);
    kfree(c);

    klog("Heap stats: used=%lu free=%lu\n",
         (unsigned long)kheap_used_bytes(),
         (unsigned long)kheap_free_bytes());
}

void kmain(void) {
    klog_init();
    klog("Sol OS kernel booting...\n");

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        klog("PANIC: no framebuffer provided by bootloader\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    klog("Framebuffer: %ux%u, %u bpp, pitch %u\n",
         (unsigned)fb->width, (unsigned)fb->height,
         (unsigned)fb->bpp, (unsigned)fb->pitch);

    fb_init(fb);

    if (memmap_request.response != NULL) {
        klog("Memory map entries: %u\n",
             (unsigned)memmap_request.response->entry_count);
    }

    klog("Sol OS: framebuffer initialized.\n");

    gdt_init();
    klog("GDT loaded.\n");

    idt_init();
    klog("IDT loaded.\n");

    pic_remap();
    klog("PIC remapped (IRQs now at vectors 32-47).\n");

    uint64_t hhdm_offset = 0;

    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
        pmm_init(memmap_request.response, hhdm_offset);
        kheap_init(hhdm_offset, memmap_request.response);
        pmm_selftest();
        heap_selftest();
    } else {
        klog("PANIC: no HHDM response from bootloader\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    uint32_t num_devs = pci_init();
    klog("PCI: initialization complete (%u device(s)).\n", num_devs);

    __asm__ volatile ("sti");   /* enable interrupts — nothing unmasked
                                   yet, so this is safe before the
                                   drivers below finish setting up */

    timer_init(100);            /* 100 Hz tick */
    klog("Timer initialized at 100 Hz.\n");

    keyboard_init();
    klog("Keyboard initialized. Type something (echoed to serial):\n");

    mouse_init();
    klog("Mouse initialized (IRQ12). Move it in QEMU.\n");

    virtio_input_init(hhdm_offset);
    klog("VirtIO input initialized.\n");

    desktop_init(fb, hhdm_offset);

    /* Main loop: echo typed characters to serial, let the desktop
     * drain input and run the UI, and log a heartbeat every ~5
     * seconds to prove the timer, keyboard, and both mice are still
     * alive. */
    uint64_t last_heartbeat = 0;
    uint64_t last_mouse_pkts = 0;
    uint64_t last_virtio_events = 0;
    for (;;) {
        char c;
        if (keyboard_read_char(&c)) {
            klog("%c", c);
        }

        desktop_poll();

        int32_t dx, dy;
        uint8_t btns;
        if (mouse_get_delta(&dx, &dy, &btns)) {
            klog("[mouse] dx=%d dy=%d buttons=%x\n",
                 (int)dx, (int)dy, (unsigned)btns);
        }

        uint64_t t = timer_get_ticks();
        if (t - last_heartbeat >= 500) {   /* 500 ticks @ 100Hz = 5s */
            uint64_t mouse_pkts = mouse_packet_count();
            uint64_t virtio_events = virtio_input_event_count();
            klog("\n[heartbeat] ticks=%u mouse_irqs=%lu mouse_packets=%lu "
                 "(+%lu) virtio_irqs=%lu virtio_events=%lu (+%lu) "
                 "virtio_key=%lu virtio_rel=%lu virtio_dropped=%lu "
                 "heap_free=%lu\n",
                 (unsigned)t, (unsigned long)mouse_irq_count(),
                 (unsigned long)mouse_pkts,
                 (unsigned long)(mouse_pkts - last_mouse_pkts),
                 (unsigned long)virtio_input_irq_count(),
                 (unsigned long)virtio_events,
                 (unsigned long)(virtio_events - last_virtio_events),
                 (unsigned long)virtio_input_key_count(),
                 (unsigned long)virtio_input_rel_count(),
                 (unsigned long)virtio_input_dropped_count(),
                 (unsigned long)kheap_free_bytes());
            last_heartbeat = t;
            last_mouse_pkts = mouse_pkts;
            last_virtio_events = virtio_events;
        }

        __asm__ volatile ("hlt");
    }
}
