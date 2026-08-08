#ifndef SOL_IDT_H
#define SOL_IDT_H

#include <stdint.h>

/* Matches the register snapshot pushed by isr_common_stub /
 * irq_common_stub in interrupts.asm before calling into C. Order
 * matters — it must mirror the push order in the assembly exactly.
 *
 * NOTE: no rsp/ss fields. The CPU only pushes SS:RSP automatically
 * on a privilege-level change (ring 3 -> ring 0); since everything
 * currently runs in ring 0, interrupts land here without them.
 * Revisit this struct (and interrupts.asm) when Phase 4 adds user
 * mode and interrupts can arrive from ring 3. */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags;
};

void idt_init(void);

/* Installs a C handler for interrupt vector `n` (0-255). Used by
 * pic.c/timer.c/keyboard.c to register IRQ handlers, and available
 * for exception handlers too. */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);
void idt_register_handler(uint8_t n, interrupt_handler_t handler);

/* PCI devices commonly share IRQ lines, so a single IRQ vector may
 * need several drivers serviced. idt_register_irq_handler installs a
 * multiplexed dispatcher on vector 32+irq and appends `node` to that
 * IRQ's handler list; each entry is invoked with its context pointer,
 * then a single EOI is sent for the line. Returns 1 on success, or 0
 * if the vector is already claimed by a direct (non-multiplexed)
 * handler — the caller should then fall back to polling. */
typedef struct irq_node {
    void (*fn)(void *ctx);
    void *ctx;
    struct irq_node *next;
} irq_node_t;

int idt_register_irq_handler(uint8_t irq, void (*fn)(void *ctx), void *ctx, irq_node_t *node);

#endif /* SOL_IDT_H */
