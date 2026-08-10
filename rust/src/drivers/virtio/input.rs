//! VirtIO input driver: virtio-keyboard and virtio-mouse.
//!
//! Faithful port of `kernel/drivers/virtio/virtio_input.c` (C),
//! feeding the central `crate::input` pipeline instead of private
//! per-device buffers. Events arrive as fixed 8-byte packets
//! (virtio spec section 5.12.5.3) written by the device into recycled
//! DMA buffers; relative motion accumulates across EV_REL events and
//! is flushed as one MouseMove per EV_SYN/SYN_REPORT frame.

use super::{
    device_cfg_read8, device_cfg_write8, device_init, finish_features, queue_add_buffer,
    queue_init, queue_kick, queue_pop_used, queue_recycle, queue_size, set_driver_ok, set_feature,
    VirtioDevice, VIRTIO_DEVICE_ZERO,
};
use crate::drivers::pci;
use crate::input;
use crate::input::MouseButton;
use crate::interrupts::{idt, pic};
use crate::memory::hhdm::Hhdm;
use crate::memory::pmm;

const VIRTIO_ID_INPUT: u16 = 0x12;
const VIRTIO_PCI_MODERN_ID: u16 = 0x1040 | VIRTIO_ID_INPUT; /* 0x1052 */
const VIRTIO_PCI_LEGACY_ID: u16 = 0x1000 | VIRTIO_ID_INPUT; /* 0x1012 */
const VIRTIO_VENDOR: u16 = 0x1AF4;

const VIRTIO_INPUT_F_EVENTS: u32 = 0;

const VIRTIO_INPUT_MAX_DEVICES: usize = 4;
const VIRTIO_INPUT_MAX_QUEUE: usize = 256; /* events array: 256 * 8 = 2 KiB/page */
const VIRTIO_INPUT_MAX_NAME: usize = 48;

/* Event types (Linux input event codes). */
const EV_SYN: u16 = 0x00;
const EV_KEY: u16 = 0x01;
const EV_REL: u16 = 0x02;
const EV_ABS: u16 = 0x03;
const EV_MSC: u16 = 0x04;

const REL_X: u16 = 0;
const REL_Y: u16 = 1;
const REL_WHEEL: u16 = 8;

/* Mouse buttons (Linux BTN_*), exposed through the same EV_KEY
 * channel the keyboard uses for its keys. */
const BTN_LEFT: u16 = 0x110;
const BTN_RIGHT: u16 = 0x111;
const BTN_MIDDLE: u16 = 0x112;
const BTN_SIDE: u16 = 0x113;
const BTN_EXTRA: u16 = 0x114;
const BTN_FORWARD: u16 = 0x115;
const BTN_BACK: u16 = 0x116;

/// Fixed 8-byte event packet.
#[repr(C, packed)]
#[derive(Clone, Copy)]
struct VirtioInputEvent {
    kind: u16, /* EV_* */
    code: u16, /* KEY_* / REL_* / ABS_* / SYN_* */
    value: u32, /* key state (0/1), relative delta, or absolute */
}

struct VirtioInput {
    valid: bool,
    index: u8,
    vdev: VirtioDevice,
    name: [u8; VIRTIO_INPUT_MAX_NAME],

    /* Event buffers: a page of physical memory the device DMA-writes
     * into. Descriptor i permanently points at events[i]. */
    events: *mut VirtioInputEvent,
    events_phys: u64,

    use_irq: bool,
    irq: u8,
    irq_node: idt::IrqNode,

    saw_keys: bool,
    saw_buttons: bool,

    /* Pointer-device accumulator, flushed on SYN_REPORT. */
    acc_dx: i32,
    acc_dy: i32,

    events_total: u64,
    events_key: u64,
    events_rel: u64,
    events_dropped: u64,
    events_syn: u64,
    irq_count: u64,
}

fn irq_handler(ctx: *mut ()) {
    let vi = unsafe { &mut *(ctx as *mut VirtioInput) };
    let isr = unsafe { core::ptr::read_volatile(vi.vdev.isr) };
    if isr == 0 {
        return; /* spurious / other device on this line */
    }
    vi.irq_count += 1;
    if isr & 0x01 != 0 {
        drain(vi); /* used-buffer notification */
    }
    /* bit 0x02 = config change: nothing to handle here */
}

const VIRTIO_INPUT_ZERO: VirtioInput = VirtioInput {
    valid: false,
    index: 0,
    vdev: VIRTIO_DEVICE_ZERO,
    name: [0; VIRTIO_INPUT_MAX_NAME],
    events: core::ptr::null_mut(),
    events_phys: 0,
    use_irq: false,
    irq: 0,
    irq_node: idt::IrqNode {
        fn_ptr: irq_handler,
        ctx: core::ptr::null_mut(),
        next: core::ptr::null_mut(),
    },
    saw_keys: false,
    saw_buttons: false,
    acc_dx: 0,
    acc_dy: 0,
    events_total: 0,
    events_key: 0,
    events_rel: 0,
    events_dropped: 0,
    events_syn: 0,
    irq_count: 0,
};

static mut G_INPUTS: [VirtioInput; VIRTIO_INPUT_MAX_DEVICES] =
    [VIRTIO_INPUT_ZERO; VIRTIO_INPUT_MAX_DEVICES];

fn button_from_code(code: u16) -> Option<MouseButton> {
    match code {
        BTN_LEFT => Some(MouseButton::Left),
        BTN_RIGHT => Some(MouseButton::Right),
        BTN_MIDDLE => Some(MouseButton::Middle),
        BTN_SIDE => Some(MouseButton::Side),
        BTN_EXTRA => Some(MouseButton::Extra),
        BTN_FORWARD => Some(MouseButton::Forward),
        BTN_BACK => Some(MouseButton::Back),
        _ => None,
    }
}

fn handle_event(vi: &mut VirtioInput, ev: &VirtioInputEvent, len: u32) {
    if (len as usize) < core::mem::size_of::<VirtioInputEvent>() {
        vi.events_dropped += 1;
        return;
    }

    match ev.kind {
        EV_SYN => {
            vi.events_syn += 1;
            /* Frame terminator: flush batched relative motion. */
            if vi.acc_dx != 0 || vi.acc_dy != 0 {
                input::submit_mouse_move(vi.acc_dx, vi.acc_dy);
                vi.acc_dx = 0;
                vi.acc_dy = 0;
            }
        }
        EV_KEY => {
            vi.events_key += 1;
            if ev.code >= 0x100 {
                /* Mouse button (BTN_*), including side/extra. */
                vi.saw_buttons = true;
                if let Some(btn) = button_from_code(ev.code) {
                    input::submit_mouse_button(btn, ev.value != 0);
                }
            } else {
                if ev.value != 0 {
                    vi.saw_keys = true;
                }
                input::submit_key(ev.code, ev.value != 0);
            }
        }
        EV_REL => {
            vi.events_rel += 1;
            match ev.code {
                REL_X => vi.acc_dx += ev.value as i32,
                REL_Y => vi.acc_dy += ev.value as i32,
                REL_WHEEL => input::submit_mouse_scroll(ev.value as i32),
                _ => {}
            }
        }
        EV_ABS => {
            /* Not tracked (tablet-style absolute input). */
        }
        EV_MSC => {
            /* Not tracked. */
        }
        _ => {
            vi.events_dropped += 1;
        }
    }
    vi.events_total += 1;
}

/// Drains every used descriptor and recycles it. Called with
/// interrupts disabled (IRQ context).
fn drain(vi: &mut VirtioInput) {
    let mut pop = (0u16, 0u32);
    let mut recycled = 0;
    while queue_pop_used(&mut vi.vdev, 0, &mut pop) {
        let id = pop.0;
        if (id as usize) < VIRTIO_INPUT_MAX_QUEUE {
            unsafe {
                handle_event(vi, &*vi.events.add(id as usize), pop.1);
            }
        } else {
            vi.events_dropped += 1;
        }
        queue_recycle(&mut vi.vdev, 0, id);
        recycled += 1;
    }
    if recycled > 0 {
        queue_kick(&vi.vdev, 0);
    }
}

fn read_name(vi: &mut VirtioInput) {
    if vi.vdev.device_cfg.is_null() {
        return;
    }
    /* select = VIRTIO_INPUT_CFG_ID_NAME (1), subsel = 0 */
    device_cfg_write8(&vi.vdev, 0, 1);
    device_cfg_write8(&vi.vdev, 1, 0);
    let mut size = device_cfg_read8(&vi.vdev, 2);
    if (size as usize) > VIRTIO_INPUT_MAX_NAME - 1 {
        size = (VIRTIO_INPUT_MAX_NAME - 1) as u8;
    }
    for i in 0..size {
        vi.name[i as usize] = device_cfg_read8(&vi.vdev, 8 + i as u32);
    }
    vi.name[size as usize] = 0;
}

fn name_str(vi: &VirtioInput) -> &str {
    let len = vi
        .name
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(VIRTIO_INPUT_MAX_NAME);
    core::str::from_utf8(&vi.name[..len]).unwrap_or("")
}

/// Finds and initializes every virtio-input device on the PCI bus.
/// Returns the number of devices made ready.
pub fn init(hhdm: &Hhdm) -> usize {
    let mut found = 0;

    for i in 0..pci::device_count() {
        if found >= VIRTIO_INPUT_MAX_DEVICES {
            break;
        }
        let p = match pci::device_at(i) {
            Some(p) => p,
            None => continue,
        };
        if p.vendor != VIRTIO_VENDOR {
            continue;
        }
        if p.device_id != VIRTIO_PCI_MODERN_ID && p.device_id != VIRTIO_PCI_LEGACY_ID {
            continue;
        }

        let vi = unsafe { &mut G_INPUTS[found] };
        vi.index = found as u8;

        crate::kprintln!(
            "[rust] virtio-input: found {:x}:{:x}.{} (id {:x})",
            p.bus,
            p.dev,
            p.func,
            p.device_id
        );

        if p.device_id != VIRTIO_PCI_MODERN_ID {
            crate::kprintln!(
                "[rust] virtio-input: legacy-transitional device {:x} unsupported (modern-only)",
                p.device_id
            );
            continue;
        }

        if device_init(&mut vi.vdev, p, hhdm).is_err() {
            crate::kprintln!("[rust] virtio-input: transport init failed");
            continue;
        }

        set_feature(&mut vi.vdev, VIRTIO_INPUT_F_EVENTS);
        if finish_features(&mut vi.vdev).is_err() {
            crate::kprintln!("[rust] virtio-input: feature negotiation failed");
            continue;
        }
        crate::kprintln!(
            "[rust] virtio-input: features ok (dev {:x}:{:x})",
            vi.vdev.device_features_lo,
            vi.vdev.device_features_hi
        );

        let mut qsize = queue_size(&vi.vdev, 0);
        if (qsize as usize) > VIRTIO_INPUT_MAX_QUEUE {
            qsize = VIRTIO_INPUT_MAX_QUEUE as u16;
        }
        if queue_init(&mut vi.vdev, 0, qsize).is_err() {
            crate::kprintln!("[rust] virtio-input: queue setup failed");
            continue;
        }

        /* One page of event buffers (256 * 8 bytes = 2 KiB). */
        let events_phys = pmm::alloc_page();
        if events_phys == 0 {
            crate::kprintln!("[rust] virtio-input: no memory for event buffers");
            continue;
        }
        vi.events_phys = events_phys;
        vi.events = (hhdm.offset() + events_phys) as *mut VirtioInputEvent;

        for n in 0..qsize {
            let phys = events_phys + (n as u64) * (core::mem::size_of::<VirtioInputEvent>() as u64);
            if !queue_add_buffer(
                &mut vi.vdev,
                0,
                phys,
                core::mem::size_of::<VirtioInputEvent>() as u32,
                true,
            ) {
                break; /* pool exhausted — not expected at setup */
            }
        }
        queue_kick(&vi.vdev, 0);

        read_name(vi);

        /* Hook the IRQ line the firmware programmed for this function. */
        let irq = pci::config_read8(p.bus, p.dev, p.func, 0x3C);
        if irq != 0 && irq < 16 {
            let ctx_ptr = (&mut *vi as *mut VirtioInput) as *mut ();
            let node_ptr = &mut vi.irq_node as *mut idt::IrqNode;
            if idt::register_irq_handler(irq, irq_handler, ctx_ptr, node_ptr) {
                pic::unmask_irq(irq);
                vi.use_irq = true;
                vi.irq = irq;
            }
        }
        if vi.use_irq {
            crate::kprintln!(
                "[rust] virtio-input[{}]: '{}' irq={} queue={}",
                found,
                if name_str(vi).is_empty() { "<unnamed>" } else { name_str(vi) },
                irq,
                qsize
            );
        } else {
            crate::kprintln!(
                "[rust] virtio-input[{}]: '{}' no IRQ (polling)",
                found,
                if name_str(vi).is_empty() { "<unnamed>" } else { name_str(vi) }
            );
        }

        set_driver_ok(&vi.vdev);
        vi.valid = true;
        found += 1;
    }

    crate::kprintln!("[rust] virtio-input: {} device(s) ready", found);
    if found > 0 {
        crate::kprintln!("[rust] VirtIO input initialized");
    }
    found
}

pub fn device_count() -> usize {
    let mut n = 0;
    for i in 0..VIRTIO_INPUT_MAX_DEVICES {
        if unsafe { G_INPUTS[i].valid } {
            n += 1;
        }
    }
    n
}

pub fn irq_count() -> u64 {
    let mut total = 0;
    for i in 0..VIRTIO_INPUT_MAX_DEVICES {
        if unsafe { G_INPUTS[i].valid } {
            total += unsafe { G_INPUTS[i].irq_count };
        }
    }
    total
}

pub fn event_count() -> u64 {
    let mut total = 0;
    for i in 0..VIRTIO_INPUT_MAX_DEVICES {
        if unsafe { G_INPUTS[i].valid } {
            total += unsafe { G_INPUTS[i].events_total };
        }
    }
    total
}

pub fn key_count() -> u64 {
    let mut total = 0;
    for i in 0..VIRTIO_INPUT_MAX_DEVICES {
        if unsafe { G_INPUTS[i].valid } {
            total += unsafe { G_INPUTS[i].events_key };
        }
    }
    total
}

pub fn dropped_count() -> u64 {
    let mut total = 0;
    for i in 0..VIRTIO_INPUT_MAX_DEVICES {
        if unsafe { G_INPUTS[i].valid } {
            total += unsafe { G_INPUTS[i].events_dropped };
        }
    }
    total
}
