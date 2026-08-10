//! Interrupt Descriptor Table: the full 256-entry IDT, exception and
//! IRQ handler dispatch.
//!
//! Faithful port of `kernel/arch/x86_64/idt.c` (C), including the
//! vector layout: CPU exceptions at 0-31 and PIC IRQs at 32-47 (after
//! the remap in `pic::remap`).

use core::arch::global_asm;

use crate::interrupts::pic;

global_asm!(include_str!("stubs.s"));
global_asm!(include_str!("idt_flush.s"));

/// Register snapshot pushed by `isr_common_stub` in `stubs.s` before
/// calling the dispatcher. Order matters — it must mirror the push
/// order in the assembly exactly.
///
/// NOTE: no rsp/ss fields. The CPU only pushes SS:RSP automatically on
/// a privilege-level change (ring 3 -> ring 0); since everything
/// currently runs in ring 0, interrupts land here without them.
/// Revisit this struct (and stubs.s) when Phase 4 adds user mode and
/// interrupts can arrive from ring 3.
#[repr(C)]
pub struct InterruptFrame {
    pub r15: u64,
    pub r14: u64,
    pub r13: u64,
    pub r12: u64,
    pub r11: u64,
    pub r10: u64,
    pub r9: u64,
    pub r8: u64,
    pub rbp: u64,
    pub rdi: u64,
    pub rsi: u64,
    pub rdx: u64,
    pub rcx: u64,
    pub rbx: u64,
    pub rax: u64,
    pub int_no: u64,
    pub err_code: u64,
    pub rip: u64,
    pub cs: u64,
    pub rflags: u64,
}

/// Installs a handler for interrupt vector `n` (0-255). Used by
/// `timer` to register IRQ handlers, and available for exception
/// handlers too.
pub type InterruptHandler = fn(&InterruptFrame);

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct IdtEntry {
    offset_low: u16,
    selector: u16,
    ist: u8,
    type_attr: u8,
    offset_mid: u16,
    offset_high: u32,
    zero: u32,
}

/// Packed IDT pointer handed to `lidt`.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct IdtPtr {
    limit: u16,
    base: u64,
}

const IDT_ENTRIES: usize = 256;
const KERNEL_CODE_SELECTOR: u16 = 0x08; /* kernel code selector from gdt.rs */
const GATE_INTERRUPT: u8 = 0x8E; /* present, ring 0, 64-bit interrupt gate */

static mut IDT: [IdtEntry; IDT_ENTRIES] = [IdtEntry {
    offset_low: 0,
    selector: 0,
    ist: 0,
    type_attr: 0,
    offset_mid: 0,
    offset_high: 0,
    zero: 0,
}; IDT_ENTRIES];

static mut IDTP: IdtPtr = IdtPtr { limit: 0, base: 0 };
static mut HANDLERS: [Option<InterruptHandler>; IDT_ENTRIES] = [None; IDT_ENTRIES];

/* Defined in `idt_flush.s`: loads `idtp` via lidt. */
extern "C" {
    fn idt_flush(idtp_addr: *const IdtPtr);
}

/* Raw ISR/IRQ entry points defined in `stubs.s` — 32 CPU exception
 * stubs (0-31) and 16 IRQ stubs (32-47, after PIC remap). The tables
 * hold the stub addresses. */
extern "C" {
    static isr_stub_table: [u64; 32];
    static irq_stub_table: [u64; 16];
}

fn exception_name(n: u64) -> &'static str {
    const NAMES: [&'static str; 32] = [
        "Divide-by-zero",
        "Debug",
        "NMI",
        "Breakpoint",
        "Overflow",
        "Bound Range Exceeded",
        "Invalid Opcode",
        "Device Not Available",
        "Double Fault",
        "Coprocessor Segment Overrun",
        "Invalid TSS",
        "Segment Not Present",
        "Stack-Segment Fault",
        "General Protection Fault",
        "Page Fault",
        "Reserved",
        "x87 Floating-Point Exception",
        "Alignment Check",
        "Machine Check",
        "SIMD Floating-Point Exception",
        "Virtualization Exception",
        "Control Protection Exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor Injection Exception",
        "VMM Communication Exception",
        "Security Exception",
        "Reserved",
    ];
    if n < 32 {
        NAMES[n as usize]
    } else {
        "Unknown"
    }
}

/// Default handler for exceptions nothing has registered a specific
/// handler for yet — logs diagnostics and halts rather than silently
/// triple-faulting into a reboot.
fn default_exception_handler(f: &InterruptFrame) -> ! {
    let cr2: u64;
    unsafe {
        core::arch::asm!(
            "mov {}, cr2",
            out(reg) cr2,
            options(nomem, nostack, preserves_flags)
        );
    }
    crate::kprintln!(
        "\n--- PANIC: unhandled exception {} ({}) ---",
        f.int_no,
        exception_name(f.int_no)
    );
    crate::kprintln!(
        "err_code={:#x} rip={:#x} cs={:#x} rflags={:#x} cr2={:#x}",
        f.err_code,
        f.rip,
        f.cs,
        f.rflags,
        cr2
    );
    crate::kernel::halt();
}

/// Called from `stubs.s`'s common stub after it has pushed a full
/// register frame. Dispatches to whatever handler was registered for
/// this vector, or the default panic handler.
#[no_mangle]
pub extern "C" fn interrupt_dispatch(frame: *mut InterruptFrame) {
    let n = unsafe { (*frame).int_no };
    let handler = if n < IDT_ENTRIES as u64 {
        unsafe { HANDLERS[n as usize] }
    } else {
        None
    };
    match handler {
        Some(h) => h(unsafe { &*frame }),
        None => default_exception_handler(unsafe { &*frame }),
    }
}

/// Installs a handler for interrupt vector `n` (0-255).
pub fn register_handler(n: u8, handler: InterruptHandler) {
    unsafe { HANDLERS[n as usize] = Some(handler) };
}

/* ---- shared IRQ line multiplexer (for PCI devices) ---- */

/// Context-driven IRQ handler invoked by the multiplexer.
pub type IrqHandler = fn(ctx: *mut ());

/// One entry in a per-IRQ handler list. Owned by the registering
/// driver (it must outlive the registration).
pub struct IrqNode {
    pub fn_ptr: IrqHandler,
    pub ctx: *mut (),
    pub next: *mut IrqNode,
}

const IRQ_COUNT: usize = 16;

static mut IRQ_NODES: [*mut IrqNode; IRQ_COUNT] = [core::ptr::null_mut(); IRQ_COUNT];

fn irq_generic_dispatch(frame: &InterruptFrame) {
    let irq = (frame.int_no - 32) as usize;
    if irq < IRQ_COUNT {
        unsafe {
            let mut n = IRQ_NODES[irq];
            while !n.is_null() {
                ((*n).fn_ptr)((*n).ctx);
                n = (*n).next;
            }
        }
    }
    pic::send_eoi(irq as u8);
}

/// PCI devices commonly share IRQ lines, so a single IRQ vector may
/// need several drivers serviced. Installs the multiplexed dispatcher
/// on vector 32+irq (unless a direct handler like the timer,
/// keyboard, or mouse already owns it) and prepends `node` to that
/// IRQ's handler list. Returns true on success; on failure the caller
/// should fall back to polling.
pub fn register_irq_handler(irq: u8, fn_ptr: IrqHandler, ctx: *mut (), node: *mut IrqNode) -> bool {
    if irq >= IRQ_COUNT as u8 {
        return false;
    }
    unsafe {
        let existing = HANDLERS[32 + irq as usize];
        if let Some(h) = existing {
            /* Function-pointer identity is reliable here: LTO with a
             * single codegen unit gives every function a stable, unique
             * address. (Cast to usize to sidestep the unstable-FnPtr
             * lint that `==` on fn pointers triggers.) */
            if h as usize != (irq_generic_dispatch as InterruptHandler) as usize {
                return false;
            }
        }
        (*node).fn_ptr = fn_ptr;
        (*node).ctx = ctx;
        (*node).next = IRQ_NODES[irq as usize];
        IRQ_NODES[irq as usize] = node;
        HANDLERS[32 + irq as usize] = Some(irq_generic_dispatch);
    }
    true
}

fn set_entry(i: usize, addr: u64, type_attr: u8) {
    unsafe {
        IDT[i].offset_low = (addr & 0xFFFF) as u16;
        IDT[i].selector = KERNEL_CODE_SELECTOR;
        IDT[i].ist = 0;
        IDT[i].type_attr = type_attr;
        IDT[i].offset_mid = ((addr >> 16) & 0xFFFF) as u16;
        IDT[i].offset_high = ((addr >> 32) & 0xFFFFFFFF) as u32;
        IDT[i].zero = 0;
    }
}

/// Builds the 256-entry IDT (exceptions at 0-31, PIC IRQs at 32-47)
/// and loads it. Call after `gdt::init`.
pub fn init() {
    unsafe {
        IDTP.limit = (core::mem::size_of::<IdtEntry>() * IDT_ENTRIES - 1) as u16;
        IDTP.base = core::ptr::addr_of!(IDT) as u64;
    }

    for i in 0..32 {
        let addr = unsafe { *isr_stub_table.as_ptr().add(i) };
        set_entry(i, addr, GATE_INTERRUPT);
    }
    for i in 0..16 {
        let addr = unsafe { *irq_stub_table.as_ptr().add(i) };
        set_entry(32 + i, addr, GATE_INTERRUPT);
    }

    unsafe { idt_flush(core::ptr::addr_of!(IDTP)) };
}
