//! VirtIO network driver: one NIC, RX/TX split virtqueues, ARP and
//! ICMP echo auto-replies.
//!
//! Faithful port of `kernel/drivers/virtio/virtio_net.c` (C). The
//! driver is poll/IRQ hybrid: the desktop poll loop drains both
//! virtqueues through `poll()`, and the IRQ handler (once registered)
//! does the same on used-buffer notifications.

use core::cell::UnsafeCell;

use super::{
    device_cfg_read8, device_init, finish_features, queue_add_buffer, queue_alloc_desc,
    queue_desc_fill, queue_init, queue_kick, queue_pop_used, queue_recycle, queue_size,
    queue_submit, set_driver_ok, set_feature, VirtioDevice, VIRTIO_DEVICE_ZERO,
};
use crate::drivers::pci::{self, PciDevice};
use crate::interrupts::{idt, pic};
use crate::memory::hhdm::Hhdm;
use crate::memory::pmm;

/* Virtio-net feature bits. */
const VIRTIO_NET_F_MAC: u32 = 5;
const VIRTIO_NET_F_MRG_RXBUF: u32 = 15;
const VIRTIO_NET_F_STATUS: u32 = 16;

/* QEMU always writes the 12-byte mergeable header on the modern
 * transport, so MRG_RXBUF is negotiated to stay spec-aligned. */
const VIRTIO_NET_HDR_SIZE: usize = 12;
const VIRTIO_NET_MTU: usize = 1514;
const VIRTIO_NET_RX_LEN: usize = 1526; /* MTU + header */

const VIRTIO_NET_S_LINK_UP: u16 = 0x01;

const VIRTIO_NET_RX_QUEUE: u16 = 0;
const VIRTIO_NET_TX_QUEUE: u16 = 1;
const VIRTIO_NET_MAX_QUEUE: u16 = 256;
const VIRTIO_NET_INBOX: usize = 16;

/* ---- protocol constants ---- */

const ETHERTYPE_IPV4: u16 = 0x0800;
const ETHERTYPE_ARP: u16 = 0x0806;
const ETH_HLEN: usize = 14;
const IPV4_HLEN: usize = 20;
const ICMP_ECHO_REPLY: u8 = 0;
const ICMP_ECHO_REQ: u8 = 8;

fn get_be16(p: &[u8]) -> u16 {
    ((p[0] as u16) << 8) | p[1] as u16
}

fn get_be32(p: &[u8]) -> u32 {
    ((get_be16(p) as u32) << 16) | get_be16(&p[2..]) as u32
}

fn put_be16(p: &mut [u8], v: u16) {
    p[0] = (v >> 8) as u8;
    p[1] = v as u8;
}

fn ip_checksum(data: &[u8]) -> u16 {
    let mut sum: u32 = 0;
    let mut i = 0;
    let mut len = data.len();
    while len >= 2 {
        sum += get_be16(&data[i..]) as u32;
        i += 2;
        len -= 2;
    }
    if len > 0 {
        sum += (data[i] as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

struct Net {
    present: bool,
    use_irq: bool,
    irq: u8,
    hhdm: u64,
    vdev: VirtioDevice,
    mac: [u8; 6],
    ip: [u8; 4],
    status: u16,
    irq_node: UnsafeCell<idt::IrqNode>,

    rx_phys: [u64; VIRTIO_NET_MAX_QUEUE as usize],
    tx_phys: [u64; VIRTIO_NET_MAX_QUEUE as usize],
    inbox_phys: [u64; VIRTIO_NET_INBOX],
    inbox_len: [u16; VIRTIO_NET_INBOX],
    inbox_head: u16,
    inbox_count: u16,

    rx_packets: u64,
    tx_packets: u64,
    rx_dropped: u64,
    tx_dropped: u64,
    irq_count: u64,
}

unsafe impl Sync for UnsafeSyncNet {}
struct UnsafeSyncNet(UnsafeCell<Net>);

impl UnsafeSyncNet {
    /// Caller guarantees exclusive access via cli (IRQ handlers only
    /// run between polls, and polls run with interrupts disabled).
    #[allow(clippy::mut_from_ref)]
    unsafe fn get(&self) -> &mut Net {
        unsafe { &mut *self.0.get() }
    }
}

static G_NET: UnsafeSyncNet = UnsafeSyncNet(UnsafeCell::new(Net {
    present: false,
    use_irq: false,
    irq: 0,
    hhdm: 0,
    vdev: VIRTIO_DEVICE_ZERO,
    mac: [0; 6],
    ip: [10, 0, 2, 15],
    status: 0,
    irq_node: UnsafeCell::new(idt::IrqNode {
        fn_ptr: irq_handler,
        ctx: core::ptr::null_mut(),
        next: core::ptr::null_mut(),
    }),
    rx_phys: [0; VIRTIO_NET_MAX_QUEUE as usize],
    tx_phys: [0; VIRTIO_NET_MAX_QUEUE as usize],
    inbox_phys: [0; VIRTIO_NET_INBOX],
    inbox_len: [0; VIRTIO_NET_INBOX],
    inbox_head: 0,
    inbox_count: 0,
    rx_packets: 0,
    tx_packets: 0,
    rx_dropped: 0,
    tx_dropped: 0,
    irq_count: 0,
}));

fn irq_handler(_ctx: *mut ()) {
    let g = unsafe { G_NET.get() };
    let isr = unsafe { core::ptr::read_volatile(g.vdev.isr) };
    if isr == 0 {
        return; /* spurious / other device on this line */
    }
    g.irq_count += 1;
    drain_raw(g);
}

/// Stages one frame copied out of RX slot `id` into the inbox ring.
fn stage_rx(g: &mut Net, id: u16, len: u32) {
    let buf_vaddr = (g.hhdm + g.rx_phys[id as usize]) as usize;

    if (len as usize) <= VIRTIO_NET_HDR_SIZE || len as usize > VIRTIO_NET_RX_LEN {
        g.rx_dropped += 1;
        return;
    }
    let payload = len as usize - VIRTIO_NET_HDR_SIZE;
    if payload > VIRTIO_NET_MTU {
        g.rx_dropped += 1;
        return;
    }

    if g.inbox_count as usize == VIRTIO_NET_INBOX {
        /* drop oldest */
        g.inbox_head = (g.inbox_head + 1) % VIRTIO_NET_INBOX as u16;
        g.inbox_count -= 1;
        g.rx_dropped += 1;
    }

    let slot = ((g.inbox_head + g.inbox_count) % VIRTIO_NET_INBOX as u16) as usize;
    unsafe {
        core::ptr::copy_nonoverlapping(
            (buf_vaddr + VIRTIO_NET_HDR_SIZE) as *const u8,
            (g.hhdm + g.inbox_phys[slot]) as *mut u8,
            payload,
        );
    }
    g.inbox_len[slot] = payload as u16;
    g.inbox_count += 1;
    g.rx_packets += 1;

    /* Auto-answer ARP + ICMP echo requests for our address. */
    let frame =
        unsafe { core::slice::from_raw_parts((g.hhdm + g.inbox_phys[slot]) as *const u8, payload) };
    handle_frame(g, frame);
}

/// Drains both virtqueues. Caller serializes (interrupts off or IRQ).
fn drain_raw(g: &mut Net) {
    let mut pop = (0u16, 0u32);

    /* TX completions: release the slot back to the free pool. */
    while queue_pop_used(&mut g.vdev, VIRTIO_NET_TX_QUEUE, &mut pop) {
        free_desc(&mut g.vdev, VIRTIO_NET_TX_QUEUE, pop.0);
        g.tx_packets += 1;
    }

    /* RX completions: stage the payload, recycle the slot. */
    let mut recycled = false;
    while queue_pop_used(&mut g.vdev, VIRTIO_NET_RX_QUEUE, &mut pop) {
        stage_rx(g, pop.0, pop.1);
        queue_recycle(&mut g.vdev, VIRTIO_NET_RX_QUEUE, pop.0);
        recycled = true;
    }
    if recycled {
        queue_kick(&g.vdev, VIRTIO_NET_RX_QUEUE);
    }
}

/// Returns a used TX descriptor to the queue's free pool.
fn free_desc(d: &mut VirtioDevice, qidx: u16, desc_id: u16) {
    let vq = &mut d.vq[qidx as usize];
    unsafe {
        (*vq.desc.add(desc_id as usize)).next = vq.free_head;
    }
    vq.free_head = desc_id;
    vq.free_count += 1;
}

/// Drains RX/TX completions. Safe to call from the main loop.
pub fn poll() {
    let g = unsafe { G_NET.get() };
    if !g.present {
        return;
    }
    unsafe { crate::interrupts::disable() };
    drain_raw(g);
    unsafe { crate::interrupts::enable() };
}

/* ---- protocol handling (ARP + ICMP auto-replies) ---- */

fn my_ip32(g: &Net) -> u32 {
    ((g.ip[0] as u32) << 24) | ((g.ip[1] as u32) << 16) | ((g.ip[2] as u32) << 8) | g.ip[3] as u32
}

fn handle_frame(g: &mut Net, frame: &[u8]) {
    if frame.len() < ETH_HLEN {
        return;
    }
    let ethertype = get_be16(&frame[12..]);
    let p = &frame[ETH_HLEN..];

    /* IPv4: answer ICMP echo requests for our own address. */
    if ethertype == ETHERTYPE_IPV4 && p.len() >= IPV4_HLEN {
        let ihl_bytes = ((p[0] & 0x0F) as usize) * 4;
        if ihl_bytes < IPV4_HLEN || ihl_bytes > p.len() {
            return;
        }
        if p[9] == 0x01 /* ICMP */ && get_be32(&p[16..]) == my_ip32(g) {
            if p.len() < ihl_bytes + 8 {
                return;
            }
            let icmp = &p[ihl_bytes..];
            if icmp[0] != ICMP_ECHO_REQ {
                return;
            }
            let mut icmp_len = p.len() - ihl_bytes;
            if icmp_len > 256 {
                icmp_len = 256;
            }

            let mut out = [0u8; ETH_HLEN + IPV4_HLEN + 256];
            out[ETH_HLEN..ETH_HLEN + ihl_bytes].copy_from_slice(&p[..ihl_bytes]);
            out[ETH_HLEN + ihl_bytes..ETH_HLEN + ihl_bytes + icmp_len]
                .copy_from_slice(&icmp[..icmp_len]);

            let o = ETH_HLEN;
            /* The reply swaps the IP addresses: source is ours,
             * destination is the sender from the request header. */
            out[o + 12..o + 16].copy_from_slice(&g.ip);
            out[o + 16..o + 20].copy_from_slice(&p[12..16]);
            out[o + 9] = 0x01;
            put_be16(&mut out[o + 10..], 0);
            let cksum = ip_checksum(&out[o..o + ihl_bytes]);
            put_be16(&mut out[o + 10..], cksum);

            let ic = ETH_HLEN + ihl_bytes;
            out[ic] = ICMP_ECHO_REPLY;
            out[ic + 1] = 0;
            put_be16(&mut out[ic + 2..], 0);
            let cksum = ip_checksum(&out[ic..ic + icmp_len]);
            put_be16(&mut out[ic + 2..], cksum);

            out[0..6].copy_from_slice(&frame[6..12]); /* eth dst = sender */
            out[6..12].copy_from_slice(&g.mac);
            put_be16(&mut out[12..], ETHERTYPE_IPV4);

            send(&out[..ETH_HLEN + ihl_bytes + icmp_len]);
        }
    }

    /* ARP: answer who-has for our address. */
    if ethertype == ETHERTYPE_ARP && p.len() >= 28 {
        let op = get_be16(&p[6..]);
        let tpa = get_be32(&p[24..]);
        if op == 1 /* REQUEST */ && tpa == my_ip32(g) {
            let mut out = [0u8; ETH_HLEN + 28];
            out[0..6].copy_from_slice(&frame[6..12]);
            out[6..12].copy_from_slice(&g.mac);
            put_be16(&mut out[12..], ETHERTYPE_ARP);

            let a = ETH_HLEN;
            put_be16(&mut out[a..], 0x0001); /* htype: ethernet */
            put_be16(&mut out[a + 2..], ETHERTYPE_IPV4);
            out[a + 4] = 6;
            out[a + 5] = 4;
            put_be16(&mut out[a + 6..], 2); /* op: REPLY */
            out[a + 8..a + 14].copy_from_slice(&g.mac);
            out[a + 14..a + 18].copy_from_slice(&g.ip);
            out[a + 18..a + 24].copy_from_slice(&p[8..14]); /* tha: requester */
            out[a + 24..a + 28].copy_from_slice(&p[14..18]); /* tpa: requester ip */

            send(&out);
        }
    }
}

/* ---- public API ---- */

/// Queues a full Ethernet frame for transmission. The frame is copied
/// into a driver-owned buffer, so `data` may be reused immediately.
/// Returns Err on a malformed frame, full TX queue, or link down.
pub fn send(data: &[u8]) -> Result<(), ()> {
    let g = unsafe { G_NET.get() };
    if !g.present {
        return Err(());
    }
    if data.len() < ETH_HLEN || data.len() > VIRTIO_NET_MTU {
        return Err(());
    }
    if g.status & VIRTIO_NET_S_LINK_UP == 0 {
        g.tx_dropped += 1;
        return Err(());
    }

    unsafe { crate::interrupts::disable() };
    let mut id: u16 = 0;
    if !queue_alloc_desc(&mut g.vdev, VIRTIO_NET_TX_QUEUE, &mut id) {
        unsafe { crate::interrupts::enable() };
        g.tx_dropped += 1;
        return Err(());
    }

    let buf = (g.hhdm + g.tx_phys[id as usize]) as *mut u8;
    unsafe {
        core::ptr::write_bytes(buf, 0, VIRTIO_NET_HDR_SIZE); /* no offloads */
        core::ptr::copy_nonoverlapping(data.as_ptr(), buf.add(VIRTIO_NET_HDR_SIZE), data.len());
    }
    queue_desc_fill(
        &g.vdev,
        VIRTIO_NET_TX_QUEUE,
        id,
        g.tx_phys[id as usize],
        (VIRTIO_NET_HDR_SIZE + data.len()) as u32,
        false,
    );
    queue_submit(&mut g.vdev, VIRTIO_NET_TX_QUEUE, id);
    queue_kick(&g.vdev, VIRTIO_NET_TX_QUEUE);
    unsafe { crate::interrupts::enable() };
    Ok(())
}

/// Copies the next received frame into `buf`, returning its length,
/// or None when the receive inbox is empty.
pub fn receive(buf: &mut [u8]) -> Option<usize> {
    unsafe { crate::interrupts::disable() };
    let g = unsafe { G_NET.get() };
    if g.inbox_count == 0 {
        unsafe { crate::interrupts::enable() };
        return None;
    }
    let slot = g.inbox_head as usize;
    let mut n = g.inbox_len[slot] as usize;
    if n > buf.len() {
        n = buf.len();
    }
    let out = buf.as_mut_ptr();
    let phys = g.inbox_phys[slot];
    unsafe {
        core::ptr::copy_nonoverlapping((g.hhdm + phys) as *const u8, out, n);
    }
    g.inbox_head = (g.inbox_head + 1) % VIRTIO_NET_INBOX as u16;
    g.inbox_count -= 1;
    unsafe { crate::interrupts::enable() };
    Some(n)
}

pub fn link_up() -> bool {
    let g = unsafe { G_NET.get() };
    g.status & VIRTIO_NET_S_LINK_UP != 0
}

pub fn present() -> bool {
    let g = unsafe { G_NET.get() };
    g.present
}

pub fn mac() -> [u8; 6] {
    let g = unsafe { G_NET.get() };
    g.mac
}

pub fn ip() -> [u8; 4] {
    let g = unsafe { G_NET.get() };
    g.ip
}

pub fn rx_packet_count() -> u64 {
    let g = unsafe { G_NET.get() };
    g.rx_packets
}

pub fn tx_packet_count() -> u64 {
    let g = unsafe { G_NET.get() };
    g.tx_packets
}

pub fn rx_dropped_count() -> u64 {
    let g = unsafe { G_NET.get() };
    g.rx_dropped
}

pub fn tx_dropped_count() -> u64 {
    let g = unsafe { G_NET.get() };
    g.tx_dropped
}

pub fn irq_count() -> u64 {
    let g = unsafe { G_NET.get() };
    g.irq_count
}

/// Probes the PCI bus for a VirtIO-net NIC and brings it up. Returns
/// true when a NIC is running.
pub fn init(hhdm: &Hhdm) -> bool {
    let g = unsafe { G_NET.get() };
    if g.present {
        return true;
    }
    g.hhdm = hhdm.offset();
    g.ip = [10, 0, 2, 15];

    let mut found: Option<&PciDevice> = None;
    for i in 0..pci::device_count() {
        let Some(p) = pci::device_at(i) else { continue };
        /* QEMU exposes the net device under several ids depending on
         * the virtio version; match on the base class instead. */
        if p.vendor == 0x1AF4 && p.base_class == 0x02 && p.sub_class == 0x00 {
            found = Some(p);
            break;
        }
    }
    let Some(p) = found else {
        crate::kprintln!("virtio-net: no NIC found on the bus");
        return false;
    };
    crate::kprintln!(
        "virtio-net: found {:x}:{:x}.{} (id {:x})",
        p.bus,
        p.dev,
        p.func,
        p.device_id
    );

    if device_init(&mut g.vdev, p, hhdm).is_err() {
        crate::kprintln!("virtio-net: transport init failed");
        return false;
    }
    if g.vdev.device_cfg.is_null() {
        crate::kprintln!("virtio-net: no device config region (cannot read MAC)");
        return false;
    }

    set_feature(&mut g.vdev, VIRTIO_NET_F_MAC);
    set_feature(&mut g.vdev, VIRTIO_NET_F_STATUS);
    set_feature(&mut g.vdev, VIRTIO_NET_F_MRG_RXBUF);
    if finish_features(&mut g.vdev).is_err() {
        crate::kprintln!("virtio-net: feature negotiation failed");
        return false;
    }
    crate::kprintln!(
        "virtio-net: dev features lo={:x} hi={:x} drv features lo={:x} hi={:x}",
        g.vdev.device_features_lo,
        g.vdev.device_features_hi,
        g.vdev.driver_features_lo,
        g.vdev.driver_features_hi
    );

    /* Read the MAC from the device config (offset 0). */
    for i in 0..6u32 {
        g.mac[i as usize] = device_cfg_read8(&g.vdev, i);
    }
    crate::kprintln!(
        "virtio-net: mac {:x}:{:x}:{:x}:{:x}:{:x}:{:x}",
        g.mac[0],
        g.mac[1],
        g.mac[2],
        g.mac[3],
        g.mac[4],
        g.mac[5]
    );

    /* Set up RX (0) and TX (1). */
    let mut rx_size = queue_size(&g.vdev, VIRTIO_NET_RX_QUEUE);
    let mut tx_size = queue_size(&g.vdev, VIRTIO_NET_TX_QUEUE);
    if rx_size > VIRTIO_NET_MAX_QUEUE {
        rx_size = VIRTIO_NET_MAX_QUEUE;
    }
    if tx_size > VIRTIO_NET_MAX_QUEUE {
        tx_size = VIRTIO_NET_MAX_QUEUE;
    }
    crate::kprintln!("virtio-net: device offers rx={} tx={}", rx_size, tx_size);

    if queue_init(&mut g.vdev, VIRTIO_NET_RX_QUEUE, rx_size).is_err()
        || queue_init(&mut g.vdev, VIRTIO_NET_TX_QUEUE, tx_size).is_err()
    {
        crate::kprintln!("virtio-net: virtqueue setup failed");
        return false;
    }

    /* RX slots: one page each, seeded with the net header + room for
     * a full MTU frame. TX slots: one page each for outgoing frames. */
    for n in 0..rx_size {
        let page = pmm::alloc_page();
        if page == 0 {
            crate::kprintln!("virtio-net: out of memory seeding RX");
            return false;
        }
        g.rx_phys[n as usize] = page;
        if !queue_add_buffer(&mut g.vdev, VIRTIO_NET_RX_QUEUE, page, VIRTIO_NET_RX_LEN as u32, true)
        {
            break;
        }
    }
    queue_kick(&g.vdev, VIRTIO_NET_RX_QUEUE);

    for n in 0..tx_size {
        let page = pmm::alloc_page();
        if page == 0 {
            crate::kprintln!("virtio-net: out of memory seeding TX");
            return false;
        }
        g.tx_phys[n as usize] = page;
    }

    for n in 0..VIRTIO_NET_INBOX {
        let page = pmm::alloc_page();
        if page == 0 {
            break;
        }
        g.inbox_phys[n] = page;
    }

    /* Poll the link status (device config, offset 6). */
    g.status = device_cfg_read8(&g.vdev, 6) as u16 | ((device_cfg_read8(&g.vdev, 7) as u16) << 8);
    crate::kprintln!(
        "virtio-net: link {} (status {:x})",
        if g.status & VIRTIO_NET_S_LINK_UP != 0 {
            "UP"
        } else {
            "down"
        },
        g.status
    );

    set_driver_ok(&g.vdev);

    /* Hook the IRQ line the firmware programmed for this function. */
    let irq = pci::config_read8(p.bus, p.dev, p.func, 0x3C);
    if irq != 0 && irq < 16 {
        let node = unsafe { &mut *g.irq_node.get() };
        if idt::register_irq_handler(irq, irq_handler, core::ptr::null_mut(), node) {
            pic::unmask_irq(irq);
            g.use_irq = true;
            g.irq = irq;
        }
    }
    crate::kprintln!(
        "virtio-net: up{} (irq={}) ip 10.0.2.15",
        if g.use_irq { " with IRQ" } else { " (polling)" },
        g.irq
    );

    g.present = true;
    true
}
