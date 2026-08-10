# Sol OS Rust kernel - interrupt entry stubs.
#
# Port of kernel/arch/x86_64/interrupts.asm (C version).
#
# x86_64 CPU exceptions are inconsistent about whether they push an
# error code: #DF(8), #TS(10), #NP(11), #SS(12), #GP(13), #PF(14),
# and #AC(17) push one; everything else doesn't. To give the Rust side
# a uniform interrupt_frame, every stub below pushes a dummy 0 error
# code for vectors that don't get one from the CPU, so the stack layout
# is identical either way by the time isr_common_stub runs.
#
# Intel syntax (the LLVM integrated assembler's default for this
# target); directives are GNU as (`.section`, `.macro`, `.quad`).

.section .text

.macro ISR_NOERR n
.global isr\n
isr\n:
    push 0            # dummy error code
    push \n           # interrupt number
    jmp isr_common_stub
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    push \n           # CPU already pushed the real error code here
    jmp isr_common_stub
.endm

.macro IRQ_STUB irq, vec
.global irq\irq
irq\irq:
    push 0            # dummy error code (IRQs never have one)
    push \vec         # interrupt number (32 + IRQ line)
    jmp isr_common_stub
.endm

# ---- CPU exception vectors 0-31 ----
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

# ---- Hardware IRQ vectors, remapped to 32-47 by pic ----
IRQ_STUB 0, 32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

# ---- Shared trampoline for both ISRs and IRQs ----
#
# Saves all general-purpose registers (matching interrupt_frame's field
# order exactly, in reverse push order since the struct is read
# top-of-stack first), calls the Rust dispatcher with RSP as the frame
# pointer, then restores everything and returns.
#
# NOTE: no swapgs / no separate kernel stack switch - both are
# unnecessary right now since interrupts only ever arrive while already
# in ring 0. Revisit when Phase 4 adds user mode.
isr_common_stub:
irq_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        # struct interrupt_frame * argument
    cld                 # SysV ABI requires DF clear on entry
    call interrupt_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         # discard int_no + err_code
    iretq

.section .data
.global isr_stub_table
isr_stub_table:
    .quad isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

.global irq_stub_table
irq_stub_table:
    .quad irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7
    .quad irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
