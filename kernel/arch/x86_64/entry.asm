; Sol OS — kernel entry stub
;
; Limine boots the kernel directly in 64-bit long mode with paging
; already enabled, so unlike a legacy Multiboot/BIOS boot path this
; stub does NOT need to set up the GDT, enable A20, switch to
; protected/long mode, etc. All it has to do is give the C code a
; known-good stack and hand off to kmain().

bits 64

section .bss
align 16
stack_bottom:
    resb 65536              ; 64 KiB kernel boot stack
stack_top:

section .text
global _start
extern kmain

_start:
    ; Limine passes no arguments in registers per the boot protocol;
    ; kmain() reads bootloader-provided data via the request pointers
    ; in limine_requests.c, not via argc/argv-style registers.
    lea rsp, [rel stack_top]
    and rsp, -16             ; ensure 16-byte alignment per SysV ABI
    xor rbp, rbp              ; mark end of stack frame for backtraces

    call kmain

    ; kmain() should never return, but if it does, halt rather than
    ; run off into undefined memory.
.hang:
    cli
    hlt
    jmp .hang
