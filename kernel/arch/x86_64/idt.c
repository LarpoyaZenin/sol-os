#include "idt.h"
#include "pic.h"
#include "klog.h"
#include <stddef.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES 256

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;
static interrupt_handler_t handlers[IDT_ENTRIES];

extern void idt_flush(uint64_t idtp_addr);

/* Raw ISR/IRQ entry points defined in interrupts.asm — 32 CPU
 * exception stubs (0-31) and 16 IRQ stubs (32-47, after PIC remap). */
extern void *isr_stub_table[32];
extern void *irq_stub_table[16];

static void idt_set_entry(int i, void *handler_fn, uint8_t type_attr) {
    uint64_t addr = (uint64_t)handler_fn;
    idt[i].offset_low  = (uint16_t)(addr & 0xFFFF);
    idt[i].selector    = 0x08;        /* kernel code selector from gdt.c */
    idt[i].ist         = 0;
    idt[i].type_attr   = type_attr;   /* 0x8E = present, ring 0, 64-bit interrupt gate */
    idt[i].offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF);
    idt[i].offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF);
    idt[i].zero        = 0;
}

static const char *exception_name(uint64_t n) {
    static const char *names[32] = {
        "Divide-by-zero", "Debug", "NMI", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Invalid Opcode",
        "Device Not Available", "Double Fault", "Coprocessor Segment Overrun",
        "Invalid TSS", "Segment Not Present", "Stack-Segment Fault",
        "General Protection Fault", "Page Fault", "Reserved",
        "x87 Floating-Point Exception", "Alignment Check", "Machine Check",
        "SIMD Floating-Point Exception", "Virtualization Exception",
        "Control Protection Exception", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor Injection Exception", "VMM Communication Exception",
        "Security Exception", "Reserved"
    };
    if (n < 32) return names[n];
    return "Unknown";
}

/* Default handler for exceptions nothing has registered a specific
 * handler for yet — logs diagnostics and halts rather than silently
 * triple-faulting into a reboot, per the Phase 2 goal in the README. */
static void default_exception_handler(struct interrupt_frame *f) {
    uint64_t cr2 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    klog("\n--- PANIC: unhandled exception %d (%s) ---\n",
         (int)f->int_no, exception_name(f->int_no));
    klog("err_code=%lx rip=%lx cs=%lx rflags=%lx cr2=%lx\n",
         (unsigned long)f->err_code, (unsigned long)f->rip,
         (unsigned long)f->cs, (unsigned long)f->rflags,
         (unsigned long)cr2);
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

/* Called from interrupts.asm's common stub after it has pushed a
 * full register frame. Dispatches to whatever handler was
 * registered for this vector, or the default panic handler. */
void interrupt_dispatch(struct interrupt_frame *frame) {
    interrupt_handler_t h = handlers[frame->int_no];
    if (h != NULL) {
        h(frame);
    } else {
        default_exception_handler(frame);
    }
}

void idt_register_handler(uint8_t n, interrupt_handler_t handler) {
    handlers[n] = handler;
}

/* ---- shared IRQ line multiplexer (for PCI devices) ---- */

#define IRQ_COUNT 16

static irq_node_t *irq_nodes[IRQ_COUNT];

static void irq_generic_dispatch(struct interrupt_frame *frame) {
    uint8_t irq = frame->int_no - 32;
    if (irq < IRQ_COUNT) {
        for (irq_node_t *n = irq_nodes[irq]; n != NULL; n = n->next) {
            n->fn(n->ctx);
        }
    }
    pic_send_eoi(irq);
}

int idt_register_irq_handler(uint8_t irq, void (*fn)(void *ctx), void *ctx, irq_node_t *node) {
    if (irq >= IRQ_COUNT) return 0;

    /* Never steal a vector a direct handler already owns (timer,
     * keyboard, mouse, ...) — that would silently break the device. */
    interrupt_handler_t existing = handlers[32 + irq];
    if (existing != NULL && existing != irq_generic_dispatch) {
        return 0;
    }

    node->fn = fn;
    node->ctx = ctx;
    node->next = irq_nodes[irq];
    irq_nodes[irq] = node;

    idt_register_handler(32 + irq, irq_generic_dispatch);
    return 1;
}

void idt_init(void) {
    idtp.limit = (uint16_t)(sizeof(struct idt_entry) * IDT_ENTRIES - 1);
    idtp.base  = (uint64_t)&idt;

    for (int i = 0; i < 32; i++) {
        idt_set_entry(i, isr_stub_table[i], 0x8E);
    }
    for (int i = 0; i < 16; i++) {
        idt_set_entry(32 + i, irq_stub_table[i], 0x8E);
    }

    idt_flush((uint64_t)&idtp);
}
