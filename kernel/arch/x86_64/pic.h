#ifndef SOL_PIC_H
#define SOL_PIC_H

#include <stdint.h>

/* Remaps the legacy 8259 PIC's IRQ lines from their power-on default
 * (0x08-0x0F, which collides with CPU exception vectors) to 0x20-0x2F
 * (32-47), matching the irq_stub_table layout in interrupts.asm.
 * Must run before idt_init() registers IRQ handlers and before
 * interrupts are enabled — an unmapped PIC firing IRQ0 would
 * otherwise land on vector 8, which the CPU reads as a double fault. */
void pic_remap(void);

/* Sends the End-Of-Interrupt signal for IRQ line `irq` (0-15). Every
 * IRQ handler must call this, or the PIC will never signal that IRQ
 * line again. */
void pic_send_eoi(uint8_t irq);

/* Unmasks (enables) a single IRQ line. Called by a driver's init
 * function once its handler is registered — masking everything
 * except what's actually driven keeps unhandled IRQs from firing
 * into the default panic handler. */
void pic_unmask_irq(uint8_t irq);

#endif /* SOL_PIC_H */
