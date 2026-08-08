#include "mouse.h"
#include "idt.h"
#include "pic.h"

/* PS/2 mouse driver (standard 8042 auxiliary-device protocol).
 *
 * The mouse delivers 3-byte packets on IRQ12 (vector 44 after the
 * PIC remap): byte 0 holds button/overflow/sync bits, bytes 1 and 2
 * are signed 9-bit deltas (8 bits in the byte + overflow flag). The
 * handler assembles packets, verifies the sync bit, and accumulates
 * deltas in globals that mouse_get_delta() drains from the main
 * loop. No held-button/repeat logic yet — that comes with the cursor
 * and window manager (Phase 3). */

#define PS2_STATUS 0x64
#define PS2_DATA   0x60

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Wait until the controller's input buffer is empty (we can send). */
static int ps2_wait_input(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) return 0;
    }
    return -1;
}

/* Wait until the controller's output buffer has data (we can read). */
static int ps2_wait_output(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) return 0;
    }
    return -1;
}

static void ps2_flush(void) {
    while (inb(PS2_STATUS) & 0x01) {
        (void)inb(PS2_DATA);
    }
}

/* Send a controller command (port 0x64). */
static int ps2_send(uint8_t cmd) {
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_STATUS, cmd);
    return 0;
}

/* Send a byte to the auxiliary (mouse) device: first issue the 0xD4
 * "write to auxiliary device" controller command, then the data byte.
 * (A bare write to port 0x60 would go to the keyboard.) */
static int ps2_write_mouse(uint8_t b) {
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_STATUS, 0xD4);
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_DATA, b);
    return 0;
}

/* Write a plain byte to port 0x60 (used for the controller's own
 * command-byte write after the 0x60 command). */
static int ps2_write_device(uint8_t b) {
    if (ps2_wait_input() != 0) return -1;
    outb(PS2_DATA, b);
    return 0;
}

/* Read a byte from a device (ACK / response). */
static int ps2_read_device(uint8_t *out) {
    if (ps2_wait_output() != 0) return -1;
    *out = inb(PS2_DATA);
    return 0;
}

/* --- packet assembly --- */

static volatile uint8_t  mouse_packet[3];
static volatile int      mouse_cycle = 0;
static volatile int32_t  mouse_dx = 0;
static volatile int32_t  mouse_dy = 0;
static volatile uint8_t  mouse_buttons = 0;
static volatile uint64_t mouse_events = 0;
static volatile uint64_t mouse_packets = 0;
static volatile uint64_t mouse_irqs = 0;

static void mouse_handler(struct interrupt_frame *frame) {
    (void)frame;
    uint8_t byte = inb(PS2_DATA);
    mouse_irqs++;

    if (mouse_cycle == 0) {
        /* Sync byte: bit 3 must be set. If not, we're mid-packet —
         * drop the byte and wait for the next one. */
        if (!(byte & 0x08)) return;
        mouse_packet[0] = byte;
        mouse_cycle = 1;
    } else if (mouse_cycle == 1) {
        mouse_packet[1] = byte;
        mouse_cycle = 2;
    } else {
        mouse_packet[2] = byte;
        mouse_cycle = 0;

        int8_t dx = (int8_t)mouse_packet[1];
        int8_t dy = (int8_t)mouse_packet[2];

        /* Overflow flags in byte 0: bit 6 = x overflow, bit 7 = y. */
        if (mouse_packet[0] & 0x40) dx = 0;
        if (mouse_packet[0] & 0x80) dy = 0;

        mouse_buttons = mouse_packet[0] & 0x07;
        mouse_dx += dx;
        mouse_dy += dy;
        mouse_events++;
        mouse_packets++;
    }

    pic_send_eoi(12);
}

void mouse_init(void) {
    uint8_t d;

    /* Disable both devices while we reconfigure. */
    ps2_send(0xAD);
    ps2_wait_input();
    ps2_send(0xA7);
    ps2_wait_input();
    ps2_flush();

    /* Read the controller command byte and enable both interrupts
     * and clocks. */
    if (ps2_send(0x20) == 0 && ps2_read_device(&d) == 0) {
        d |= 0x01;    /* keyboard IRQ */
        d |= 0x02;    /* mouse IRQ */
        d |= 0x04;    /* system flag */
        d &= ~0x10;   /* enable keyboard clock */
        d &= ~0x20;   /* enable mouse clock */
        d &= ~0x40;   /* no scancode translation */
        ps2_send(0x60);
        ps2_write_device(d);
    }

    /* Enable the auxiliary device. */
    ps2_send(0xA8);
    ps2_wait_input();

    /* Set defaults, then enable data reporting. */
    if (ps2_write_mouse(0xF6) == 0) (void)ps2_read_device(&d);   /* ACK */
    if (ps2_write_mouse(0xF4) == 0) (void)ps2_read_device(&d);   /* ACK */

    /* Re-enable the keyboard. */
    ps2_send(0xAE);
    ps2_wait_input();

    idt_register_handler(44, mouse_handler);
    pic_unmask_irq(12);
}

bool mouse_get_delta(int32_t *dx, int32_t *dy, uint8_t *buttons) {
    __asm__ volatile ("cli");
    int32_t ddx = mouse_dx;
    int32_t ddy = mouse_dy;
    uint8_t b = mouse_buttons;
    uint64_t events = mouse_events;
    mouse_dx = 0;
    mouse_dy = 0;
    mouse_events = 0;
    __asm__ volatile ("sti");

    *dx = ddx;
    *dy = ddy;
    *buttons = b;
    return events > 0;
}

uint64_t mouse_packet_count(void) {
    return mouse_packets;
}

/* Number of IRQ12 interrupts the handler has serviced. */
uint64_t mouse_irq_count(void) {
    return mouse_irqs;
}
