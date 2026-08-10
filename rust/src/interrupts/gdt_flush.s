# gdt_flush(*const gdt_ptr) - rdi holds the pointer per SysV ABI.
# Loads the GDT, then reloads CS via a far return (the standard
# long-mode-safe way to reload CS, since a direct far jmp with an
# immediate segment:offset isn't position-independent-friendly), and
# reloads the data segment registers directly.
#
# Port of kernel/arch/x86_64/gdt_flush.asm (C version).

.section .text
.global gdt_flush

gdt_flush:
    lgdt [rdi]

    mov ax, 0x10          # kernel data selector (index 2 << 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    # Reload CS = 0x08 (kernel code selector, index 1 << 3) via
    # far return: push the target selector and return address,
    # then retfq pops both and reloads CS as part of the transfer.
    pop rax                # return address pushed by the `call`
                           # that got us here
    push 0x08
    push rax
    retfq
