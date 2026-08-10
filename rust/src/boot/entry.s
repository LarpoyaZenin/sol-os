# Sol OS Rust kernel - entry stub
#
# Port of kernel/arch/x86_64/entry.asm (C version). Limine boots the
# kernel directly in 64-bit long mode with paging enabled, so all we
# do is give the Rust code a known-good 64 KiB stack and hand off to
# kernel_main().
#
# Written in GNU as syntax because it is assembled by LLVM's integrated
# assembler (core::arch::global_asm!).

.section .bss
.align 16
stack_bottom:
    .zero 65536              # 64 KiB kernel boot stack
stack_top:

.section .text
.global _start
_start:
    lea rsp, [rip + stack_top]
    and rsp, -16             # ensure 16-byte alignment per SysV ABI
    xor rbp, rbp             # mark end of stack frame for backtraces

    call kernel_main

    # kernel_main() should never return, but if it does, halt rather
    # than run off into undefined memory.
.hang:
    cli
    hlt
    jmp .hang
