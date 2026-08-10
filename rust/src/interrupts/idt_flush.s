# idt_flush(*const idt_ptr) - rdi holds the pointer per SysV ABI.
# Unlike the GDT, loading a new IDT doesn't require any segment
# register reloading - just lidt.
#
# Port of kernel/arch/x86_64/idt_flush.asm (C version).

.section .text
.global idt_flush

idt_flush:
    lidt [rdi]
    ret
