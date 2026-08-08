; idt_flush(uint64_t idtp_addr) — rdi holds the pointer per SysV ABI.
; Unlike the GDT, loading a new IDT doesn't require any segment
; register reloading — just lidt.

bits 64

section .text
global idt_flush

idt_flush:
    lidt [rdi]
    ret
