#include "pic.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01

#define PIC_EOI    0x20

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Tiny delay for old hardware that needs a beat between successive
 * PIC I/O writes during the init sequence — writing to an unused
 * port is the traditional OSDev trick for this. */
static inline void io_wait(void) {
    outb(0x80, 0);
}

void pic_remap(void) {
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC1_DATA, 0x20);   /* master PIC IRQs start at vector 32 */
    io_wait();
    outb(PIC2_DATA, 0x28);   /* slave PIC IRQs start at vector 40 */
    io_wait();

    outb(PIC1_DATA, 4);      /* tell master PIC there's a slave at IRQ2 */
    io_wait();
    outb(PIC2_DATA, 2);      /* tell slave PIC its cascade identity */
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Mask everything by default — deterministic starting state,
     * rather than inheriting whatever the BIOS/Limine left behind.
     * Individual drivers (timer.c, keyboard.c) unmask their own IRQ
     * line once they're initialized and ready to handle it. Note
     * IRQ2 must stay unmasked on the master PIC since it's the
     * cascade line the slave PIC's IRQs (8-15) depend on. */
    outb(PIC1_DATA, 0xFB);   /* all masked except IRQ2 (cascade) */
    outb(PIC2_DATA, 0xFF);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

/* Unmasks (enables) a single IRQ line — called by driver init
 * functions once their handler is registered and ready. */
void pic_unmask_irq(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) & ~(uint8_t)(1 << irq);
    outb(port, value);
}
