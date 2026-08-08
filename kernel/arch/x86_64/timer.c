#include "timer.h"
#include "idt.h"
#include "pic.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_BASE_FREQ     1193182u  /* PIT's fixed input clock, Hz */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static volatile uint64_t ticks = 0;

static void timer_handler(struct interrupt_frame *frame) {
    (void)frame;
    ticks++;
    pic_send_eoi(0);
}

uint64_t timer_get_ticks(void) {
    return ticks;
}

void timer_init(uint32_t hz) {
    uint32_t divisor = PIT_BASE_FREQ / hz;

    /* 0x36 = channel 0, lobyte/hibyte access, mode 3 (square wave),
     * binary (not BCD) counting. */
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    idt_register_handler(32, timer_handler);  /* IRQ0 -> vector 32 */
    pic_unmask_irq(0);
}
