; gdt_flush(uint64_t gdtp_addr) — rdi holds the pointer per SysV ABI.
; Loads the GDT, then reloads CS via a far return (the standard
; long-mode-safe way to reload CS, since a direct far jmp with an
; immediate segment:offset isn't position-independent-friendly and
; some assemblers/toolchains handle it inconsistently in 64-bit
; mode), and reloads the data segment registers directly.

bits 64

section .text
global gdt_flush

gdt_flush:
    lgdt [rdi]

    mov ax, 0x10          ; kernel data selector (index 2 << 3)
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Reload CS = 0x08 (kernel code selector, index 1 << 3) via
    ; far return: push the target selector and return address,
    ; then retfq pops both and reloads CS as part of the transfer.
    pop rax                ; return address pushed by the `call`
                            ; that got us here
    push 0x08
    push rax
    retfq
