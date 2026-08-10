//! VirtIO "modern" (1.0) transport over PCI: capability-region
//! discovery, feature negotiation, and split virtqueues.
//!
//! Faithful port of `kernel/drivers/virtio/virtio.c` (C). The device
//! exposes four MMIO regions — common config, notify, ISR, and device
//! config — each located through a PCI vendor-specific capability
//! (VIRTIO_PCI_CAP_VNDR). Limine's HHDM only maps usable RAM, so each
//! region is mapped explicitly with `paging::map_physical` before it
//! is touched.

pub mod input;

use crate::drivers::pci::{self, PciDevice};
use crate::memory::hhdm::Hhdm;
use crate::memory::{paging, pmm};

pub const VIRTIO_PCI_CAP_VNDR: u8 = 0x09;

pub const VIRTIO_PCI_CAP_COMMON_CFG: u8 = 1;
pub const VIRTIO_PCI_CAP_NOTIFY_CFG: u8 = 2;
pub const VIRTIO_PCI_CAP_ISR_CFG: u8 = 3;
pub const VIRTIO_PCI_CAP_DEVICE_CFG: u8 = 4;

pub const VIRTIO_STATUS_ACKNOWLEDGE: u8 = 0x01;
pub const VIRTIO_STATUS_DRIVER: u8 = 0x02;
pub const VIRTIO_STATUS_DRIVER_OK: u8 = 0x04;
pub const VIRTIO_STATUS_FEATURES_OK: u8 = 0x08;
pub const VIRTIO_STATUS_FAILED: u8 = 0x80;

pub const VIRTIO_F_VERSION_1: u32 = 32;
pub const VIRTQ_DESC_F_WRITE: u16 = 2;

pub const VIRTIO_MAX_QUEUES: usize = 4;
pub const VIRTIO_MAX_QUEUE_SIZE: usize = 256;

/* Common config register offsets (virtio spec 4.1.4.3.2). */
const CC_DEVICE_FEATURE_SELECT: u32 = 0x00;
const CC_DEVICE_FEATURE: u32 = 0x04;
const CC_DRIVER_FEATURE_SELECT: u32 = 0x08;
const CC_DRIVER_FEATURE: u32 = 0x0C;
const CC_DEVICE_STATUS: u32 = 0x14;
const CC_QUEUE_SELECT: u32 = 0x16;
const CC_QUEUE_SIZE: u32 = 0x18;
const CC_QUEUE_ENABLE: u32 = 0x1C;
const CC_QUEUE_NOTIFY_OFF: u32 = 0x1E;
const CC_QUEUE_DESC: u32 = 0x20;
const CC_QUEUE_DRIVER: u32 = 0x28;
const CC_QUEUE_DEVICE: u32 = 0x30;

/* Sanity bounds on the capability chain region sizes. */
const VIRTIO_COMMON_CFG_MIN: u64 = 0x24;
const VIRTIO_NOTIFY_CAP_MIN: u64 = 0x14;
const VIRTIO_DEVICE_CFG_MIN: u64 = 0x08;

/* ---- split virtqueue structures (virtio spec section 2.6) ---- */

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VirtqDesc {
    pub addr: u64,
    pub len: u32,
    pub flags: u16,
    pub next: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VirtqAvail {
    pub flags: u16,
    pub idx: u16,
    pub ring: [u16; VIRTIO_MAX_QUEUE_SIZE],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VirtqUsedElem {
    pub id: u32,
    pub len: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct VirtqUsed {
    pub flags: u16,
    pub idx: u16,
    pub ring: [VirtqUsedElem; VIRTIO_MAX_QUEUE_SIZE],
}

#[derive(Clone, Copy)]
pub struct Virtq {
    pub queue_size: u16,
    pub free_head: u16,
    pub free_count: u16,
    pub avail_idx: u16,
    pub used_idx: u16,
    pub desc: *mut VirtqDesc,
    pub avail: *mut VirtqAvail,
    pub used: *mut VirtqUsed,
    pub desc_phys: u64,
    pub avail_phys: u64,
    pub used_phys: u64,
    pub notify_addr: usize,
}

const VIRTQ_ZERO: Virtq = Virtq {
    queue_size: 0,
    free_head: 0,
    free_count: 0,
    avail_idx: 0,
    used_idx: 0,
    desc: core::ptr::null_mut(),
    avail: core::ptr::null_mut(),
    used: core::ptr::null_mut(),
    desc_phys: 0,
    avail_phys: 0,
    used_phys: 0,
    notify_addr: 0,
};

pub struct VirtioDevice {
    pub hhdm_offset: u64,
    pub bus: u8,
    pub dev: u8,
    pub func: u8,
    pub common: *mut u8,
    pub notify: *mut u8,
    pub isr: *mut u8,
    pub device_cfg: *mut u8,
    pub notify_off_multiplier: u32,
    pub device_features_lo: u32,
    pub device_features_hi: u32,
    pub driver_features_lo: u32,
    pub driver_features_hi: u32,
    pub vq: [Virtq; VIRTIO_MAX_QUEUES],
}

pub const VIRTIO_DEVICE_ZERO: VirtioDevice = VirtioDevice {
    hhdm_offset: 0,
    bus: 0,
    dev: 0,
    func: 0,
    common: core::ptr::null_mut(),
    notify: core::ptr::null_mut(),
    isr: core::ptr::null_mut(),
    device_cfg: core::ptr::null_mut(),
    notify_off_multiplier: 0,
    device_features_lo: 0,
    device_features_hi: 0,
    driver_features_lo: 0,
    driver_features_hi: 0,
    vq: [VIRTQ_ZERO; VIRTIO_MAX_QUEUES],
};

/* ---- MMIO accessors ---- */

#[inline]
unsafe fn mmio_read32(base: *const u8, off: u32) -> u32 {
    core::ptr::read_volatile(base.add(off as usize) as *const u32)
}

#[inline]
unsafe fn mmio_read16(base: *const u8, off: u32) -> u16 {
    core::ptr::read_volatile(base.add(off as usize) as *const u16)
}

#[inline]
unsafe fn mmio_read8(base: *const u8, off: u32) -> u8 {
    core::ptr::read_volatile(base.add(off as usize))
}

#[inline]
unsafe fn mmio_write32(base: *mut u8, off: u32, val: u32) {
    core::ptr::write_volatile(base.add(off as usize) as *mut u32, val);
}

#[inline]
unsafe fn mmio_write16(base: *mut u8, off: u32, val: u16) {
    core::ptr::write_volatile(base.add(off as usize) as *mut u16, val);
}

#[inline]
unsafe fn mmio_write8(base: *mut u8, off: u32, val: u8) {
    core::ptr::write_volatile(base.add(off as usize), val);
}

/* ---- device setup ---- */

/// Physical address of PCI BAR `bar`, or None if it's not a supported
/// memory BAR.
fn bar_base(p: &PciDevice, bar: u8) -> Option<u64> {
    if bar >= 6 {
        return None;
    }
    let lo = p.bars[bar as usize];
    if lo & 1 != 0 {
        return None; /* I/O space BAR — unsupported */
    }
    let addr = if lo & 0x4 != 0 {
        /* 64-bit BAR */
        if bar + 1 >= 6 {
            return None;
        }
        ((p.bars[bar as usize + 1] as u64) << 32) | (lo as u64 & 0xFFFF_FFF0)
    } else {
        lo as u64 & 0xFFFF_FFF0
    };
    Some(addr)
}

/// Resets the device, acknowledges + DRIVER status, reads device
/// features, and records VIRTIO_F_VERSION_1 in the driver features.
/// Returns Ok(()) once the caller negotiates its own bits, calls
/// `finish_features`, sets up its queue(s), and `set_driver_ok`.
pub fn device_init(d: &mut VirtioDevice, p: &PciDevice, hhdm: &Hhdm) -> Result<(), ()> {
    d.hhdm_offset = hhdm.offset();
    d.bus = p.bus;
    d.dev = p.dev;
    d.func = p.func;
    d.notify_off_multiplier = 1;

    /* Walk the capability list, mapping each virtio region. */
    let mut off = pci::cap_first(p.bus, p.dev, p.func);
    while off != 0 {
        let id = pci::config_read8(p.bus, p.dev, p.func, off);
        if id == VIRTIO_PCI_CAP_VNDR {
            let cfg_type = pci::config_read8(p.bus, p.dev, p.func, off + 3);
            let bar = pci::config_read8(p.bus, p.dev, p.func, off + 4);
            let cap_off = pci::config_read32(p.bus, p.dev, p.func, off + 8) as u64;
            let cap_len = pci::config_read32(p.bus, p.dev, p.func, off + 12) as u64;

            let bar_base = match bar_base(p, bar) {
                Some(b) => b,
                None => {
                    crate::kprintln!(
                        "[rust] virtio: unsupported BAR {} on {:x}:{:x}.{}",
                        bar,
                        p.bus,
                        p.dev,
                        p.func
                    );
                    off = pci::cap_next(p.bus, p.dev, p.func, off);
                    continue;
                }
            };

            /* Device MMIO is outside the HHDM's usable-RAM mapping;
             * map it explicitly before accessing. */
            unsafe {
                paging::map_physical(hhdm, bar_base + cap_off, cap_len);
            }
            let vaddr = (hhdm.offset() + bar_base + cap_off) as usize;

            match cfg_type {
                VIRTIO_PCI_CAP_COMMON_CFG => {
                    if cap_len < VIRTIO_COMMON_CFG_MIN {
                        crate::kprintln!("[rust] virtio: common cfg too small ({} bytes)", cap_len);
                        return Err(());
                    }
                    d.common = vaddr as *mut u8;
                }
                VIRTIO_PCI_CAP_NOTIFY_CFG => {
                    if cap_len < VIRTIO_NOTIFY_CAP_MIN {
                        crate::kprintln!("[rust] virtio: notify cap too small ({} bytes)", cap_len);
                        return Err(());
                    }
                    d.notify = vaddr as *mut u8;
                    d.notify_off_multiplier =
                        pci::config_read32(p.bus, p.dev, p.func, off + 16);
                }
                VIRTIO_PCI_CAP_ISR_CFG => {
                    if cap_len < 1 {
                        crate::kprintln!("[rust] virtio: ISR cap too small");
                        return Err(());
                    }
                    d.isr = vaddr as *mut u8;
                }
                VIRTIO_PCI_CAP_DEVICE_CFG => {
                    if cap_len < VIRTIO_DEVICE_CFG_MIN {
                        crate::kprintln!("[rust] virtio: device cfg cap too small ({} bytes)", cap_len);
                        return Err(());
                    }
                    d.device_cfg = vaddr as *mut u8;
                }
                _ => {}
            }
        }
        off = pci::cap_next(p.bus, p.dev, p.func, off);
    }

    if d.common.is_null() || d.notify.is_null() || d.isr.is_null() {
        crate::kprintln!(
            "[rust] virtio: missing capability regions on {:x}:{:x}.{}",
            p.bus,
            p.dev,
            p.func
        );
        return Err(());
    }

    crate::kprintln!(
        "[rust] virtio: {:x}:{:x}.{} caps: common={:p} notify={:p} (mult {}) isr={:p}{}",
        p.bus,
        p.dev,
        p.func,
        d.common,
        d.notify,
        d.notify_off_multiplier,
        d.isr,
        if d.device_cfg.is_null() { " (no device cfg)" } else { "" }
    );

    /* Reset, then acknowledge + DRIVER per spec. */
    unsafe {
        mmio_write8(d.common, CC_DEVICE_STATUS, 0);
        let _ = mmio_read8(d.common, CC_DEVICE_STATUS); /* read back after reset */
        mmio_write8(d.common, CC_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
        mmio_write8(
            d.common,
            CC_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER,
        );
    }

    /* Read device features (both 32-bit words). */
    unsafe {
        mmio_write32(d.common, CC_DEVICE_FEATURE_SELECT, 0);
        d.device_features_lo = mmio_read32(d.common, CC_DEVICE_FEATURE);
        mmio_write32(d.common, CC_DEVICE_FEATURE_SELECT, 1);
        d.device_features_hi = mmio_read32(d.common, CC_DEVICE_FEATURE);
        mmio_write32(d.common, CC_DEVICE_FEATURE_SELECT, 0);
    }

    if d.device_features_hi & (1u32 << (VIRTIO_F_VERSION_1 - 32)) == 0 {
        crate::kprintln!(
            "[rust] virtio: device does not offer VIRTIO_F_VERSION_1 (features {:x} {:x})",
            d.device_features_lo,
            d.device_features_hi
        );
        device_failed(d);
        return Err(());
    }

    /* Advertise VIRTIO_F_VERSION_1 (mandatory on the modern transport). */
    d.driver_features_lo = 0;
    d.driver_features_hi = 1u32 << (VIRTIO_F_VERSION_1 - 32);
    unsafe {
        mmio_write32(d.common, CC_DRIVER_FEATURE_SELECT, 0);
        mmio_write32(d.common, CC_DRIVER_FEATURE, d.driver_features_lo);
        mmio_write32(d.common, CC_DRIVER_FEATURE_SELECT, 1);
        mmio_write32(d.common, CC_DRIVER_FEATURE, d.driver_features_hi);
        mmio_write32(d.common, CC_DRIVER_FEATURE_SELECT, 0);
    }

    Ok(())
}

/// Negotiates feature bit `bit` (driver-side) with the device.
pub fn set_feature(d: &mut VirtioDevice, bit: u32) {
    let word = bit / 32;
    let mask = 1u32 << (bit % 32);
    let value;
    if word == 0 {
        d.driver_features_lo |= mask;
        value = d.driver_features_lo;
    } else {
        d.driver_features_hi |= mask;
        value = d.driver_features_hi;
    }
    unsafe {
        mmio_write32(d.common, CC_DRIVER_FEATURE_SELECT, word);
        mmio_write32(d.common, CC_DRIVER_FEATURE, value);
        mmio_write32(d.common, CC_DRIVER_FEATURE_SELECT, 0);
    }
}

/// Writes FEATURES_OK and verifies the device accepted it.
pub fn finish_features(d: &mut VirtioDevice) -> Result<(), ()> {
    unsafe {
        mmio_write8(
            d.common,
            CC_DEVICE_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK,
        );
        let status = mmio_read8(d.common, CC_DEVICE_STATUS);
        if status & VIRTIO_STATUS_FEATURES_OK == 0 {
            crate::kprintln!("[rust] virtio: FEATURES_OK not accepted (status {:x})", status);
            device_failed(d);
            return Err(());
        }
    }
    Ok(())
}

pub fn set_driver_ok(d: &VirtioDevice) {
    unsafe {
        let status = mmio_read8(d.common, CC_DEVICE_STATUS);
        mmio_write8(d.common, CC_DEVICE_STATUS, status | VIRTIO_STATUS_DRIVER_OK);
    }
}

pub fn device_failed(d: &VirtioDevice) {
    unsafe {
        let status = mmio_read8(d.common, CC_DEVICE_STATUS);
        mmio_write8(d.common, CC_DEVICE_STATUS, status | VIRTIO_STATUS_FAILED);
    }
}

/* ---- device config access ---- */

pub fn device_cfg_read8(d: &VirtioDevice, off: u32) -> u8 {
    unsafe { core::ptr::read_volatile(d.device_cfg.add(off as usize)) }
}

pub fn device_cfg_write8(d: &VirtioDevice, off: u32, val: u8) {
    unsafe { core::ptr::write_volatile(d.device_cfg.add(off as usize), val) }
}

/* ---- virtqueues ---- */

/// Queue size the device advertises for `queue_index`.
pub fn queue_size(d: &VirtioDevice, qidx: u16) -> u16 {
    unsafe {
        mmio_write16(d.common, CC_QUEUE_SELECT, qidx);
        mmio_read16(d.common, CC_QUEUE_SIZE)
    }
}

/// Allocates and programs virtqueue `queue_index` and enables it.
/// Each ring region is page-sized for queue_size <= 256, so a single
/// physical page per region keeps the rings page-aligned.
pub fn queue_init(d: &mut VirtioDevice, qidx: u16, mut queue_size: u16) -> Result<(), ()> {
    if qidx as usize >= VIRTIO_MAX_QUEUES {
        return Err(());
    }
    if queue_size < 2 {
        crate::kprintln!(
            "[rust] virtio: queue {}: device offered an unusable size ({})",
            qidx,
            queue_size
        );
        return Err(());
    }
    if queue_size as usize > VIRTIO_MAX_QUEUE_SIZE {
        queue_size = VIRTIO_MAX_QUEUE_SIZE as u16;
    }

    let desc_phys = pmm::alloc_page();
    let avail_phys = pmm::alloc_page();
    let used_phys = pmm::alloc_page();
    if desc_phys == 0 || avail_phys == 0 || used_phys == 0 {
        crate::kprintln!("[rust] virtio: queue {}: out of memory for virtqueue", qidx);
        if desc_phys != 0 {
            pmm::free_page(desc_phys);
        }
        if avail_phys != 0 {
            pmm::free_page(avail_phys);
        }
        if used_phys != 0 {
            pmm::free_page(used_phys);
        }
        return Err(());
    }

    let hhdm_off = d.hhdm_offset;
    let vq = &mut d.vq[qidx as usize];
    vq.queue_size = queue_size;
    vq.desc_phys = desc_phys;
    vq.avail_phys = avail_phys;
    vq.used_phys = used_phys;
    vq.desc = (hhdm_off + desc_phys) as *mut VirtqDesc;
    vq.avail = (hhdm_off + avail_phys) as *mut VirtqAvail;
    vq.used = (hhdm_off + used_phys) as *mut VirtqUsed;

    /* Link every descriptor into the free pool. */
    unsafe {
        for i in 0..queue_size {
            (*vq.desc.add(i as usize)).next = i + 1;
        }
        (*vq.desc.add((queue_size - 1) as usize)).next = 0;
    }
    vq.free_head = 0;
    vq.free_count = queue_size;
    vq.avail_idx = 0;
    vq.used_idx = 0;
    unsafe {
        (*vq.avail).flags = 0;
        (*vq.avail).idx = 0;
        (*vq.used).flags = 0;
        (*vq.used).idx = 0;
    }

    unsafe {
        /* Program the device. */
        mmio_write16(d.common, CC_QUEUE_SELECT, qidx);
        mmio_write16(d.common, CC_QUEUE_SIZE, queue_size);
        mmio_write32(d.common, CC_QUEUE_DESC, desc_phys as u32);
        mmio_write32(d.common, CC_QUEUE_DESC + 4, (desc_phys >> 32) as u32);
        mmio_write32(d.common, CC_QUEUE_DRIVER, avail_phys as u32);
        mmio_write32(d.common, CC_QUEUE_DRIVER + 4, (avail_phys >> 32) as u32);
        mmio_write32(d.common, CC_QUEUE_DEVICE, used_phys as u32);
        mmio_write32(d.common, CC_QUEUE_DEVICE + 4, (used_phys >> 32) as u32);

        let notify_off = mmio_read16(d.common, CC_QUEUE_NOTIFY_OFF);
        vq.notify_addr = (d.notify as usize)
            + (notify_off as usize) * (d.notify_off_multiplier as usize);

        mmio_write16(d.common, CC_QUEUE_ENABLE, 1);

        let accepted = mmio_read16(d.common, CC_QUEUE_SIZE);
        if accepted < queue_size {
            crate::kprintln!(
                "[rust] virtio: queue {}: device accepted a smaller size ({} < {})",
                qidx,
                accepted,
                queue_size
            );
            vq.queue_size = accepted;
        }

        crate::kprintln!(
            "[rust] virtio: queue {}: {} descs (accepted {}) notify_off={}",
            qidx,
            queue_size,
            vq.queue_size,
            notify_off
        );
    }
    Ok(())
}

/// Reserves one descriptor from the free pool.
pub fn queue_alloc_desc(d: &mut VirtioDevice, qidx: u16, out_id: &mut u16) -> bool {
    if qidx as usize >= VIRTIO_MAX_QUEUES {
        return false;
    }
    let vq = &mut d.vq[qidx as usize];
    if vq.free_count == 0 {
        return false;
    }
    let id = vq.free_head;
    vq.free_head = unsafe { (*vq.desc.add(id as usize)).next };
    vq.free_count -= 1;
    *out_id = id;
    true
}

/// Fills descriptor `desc_id` (address/len/flags). `device_writes`
/// marks it write-only for the device (input event buffers).
pub fn queue_desc_fill(
    d: &VirtioDevice,
    qidx: u16,
    desc_id: u16,
    phys: u64,
    len: u32,
    device_writes: bool,
) {
    if qidx as usize >= VIRTIO_MAX_QUEUES {
        return;
    }
    let vq = &d.vq[qidx as usize];
    if desc_id >= vq.queue_size {
        return;
    }
    unsafe {
        (*vq.desc.add(desc_id as usize)).addr = phys;
        (*vq.desc.add(desc_id as usize)).len = len;
        (*vq.desc.add(desc_id as usize)).flags = if device_writes {
            VIRTQ_DESC_F_WRITE
        } else {
            0
        };
        (*vq.desc.add(desc_id as usize)).next = 0;
    }
}

/// Publishes descriptor `desc_id` to the available ring.
pub fn queue_submit(d: &mut VirtioDevice, qidx: u16, desc_id: u16) {
    if qidx as usize >= VIRTIO_MAX_QUEUES {
        return;
    }
    let vq = &mut d.vq[qidx as usize];
    if desc_id >= vq.queue_size {
        return;
    }
    unsafe {
        (*vq.avail).ring[(vq.avail_idx % vq.queue_size) as usize] = desc_id;
        core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
        vq.avail_idx += 1;
        (*vq.avail).idx = vq.avail_idx;
    }
}

/// Re-publishes a used descriptor without touching its address/len.
pub fn queue_recycle(d: &mut VirtioDevice, qidx: u16, desc_id: u16) {
    if qidx as usize >= VIRTIO_MAX_QUEUES {
        return;
    }
    let vq = &mut d.vq[qidx as usize];
    if desc_id >= vq.queue_size {
        return;
    }
    unsafe {
        (*vq.avail).ring[(vq.avail_idx % vq.queue_size) as usize] = desc_id;
        core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
        vq.avail_idx += 1;
        (*vq.avail).idx = vq.avail_idx;
    }
}

/// Publishes one descriptor for a buffer the device DMA-writes into.
pub fn queue_add_buffer(
    d: &mut VirtioDevice,
    qidx: u16,
    phys: u64,
    len: u32,
    device_writes: bool,
) -> bool {
    let mut id: u16 = 0;
    if !queue_alloc_desc(d, qidx, &mut id) {
        return false;
    }
    queue_desc_fill(d, qidx, id, phys, len, device_writes);
    queue_submit(d, qidx, id);
    true
}

/// Returns 1 and fills `out` with the next used element, or None.
pub fn queue_pop_used(d: &mut VirtioDevice, qidx: u16, out: &mut (u16, u32)) -> bool {
    if qidx as usize >= VIRTIO_MAX_QUEUES {
        return false;
    }
    let vq = &mut d.vq[qidx as usize];
    if vq.used_idx == unsafe { (*vq.used).idx } {
        return false; /* nothing new */
    }
    unsafe {
        core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
        let e = &(*vq.used).ring[(vq.used_idx % vq.queue_size) as usize];
        out.0 = e.id as u16;
        out.1 = e.len;
        vq.used_idx += 1;
    }
    true
}

/// Notifies the device that queue `qidx`'s available ring advanced.
pub fn queue_kick(d: &VirtioDevice, qidx: u16) {
    if qidx as usize >= VIRTIO_MAX_QUEUES {
        return;
    }
    let vq = &d.vq[qidx as usize];
    unsafe {
        /* Ensure the available-ring update is visible before the MMIO
         * notification (mirrors the C `mfence`). */
        core::arch::asm!("mfence", options(nostack, preserves_flags));
        core::ptr::write_volatile(vq.notify_addr as *mut u16, 0);
    }
}
