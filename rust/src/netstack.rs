//! Minimal internet client stack: ARP cache, IPv4, UDP, DNS, TCP client,
//! and HTTP/1.0 GET. Poll-driven; only one request may be in flight at
//! a time. Port of `kernel/netstack.c`.
//!
//! Call `ns_poll()` from the desktop main loop so timeouts and
//! retransmits advance.

use crate::drivers::virtio::net;
use crate::interrupts::timer;
use crate::memory::kheap;

/* ---- byte helpers ---- */

#[inline]
fn b16(p: &[u8]) -> u16 {
    ((p[0] as u16) << 8) | p[1] as u16
}

#[inline]
fn b32(p: &[u8]) -> u32 {
    ((b16(p) as u32) << 16) | b16(&p[2..]) as u32
}

#[inline]
fn w16(p: &mut [u8], v: u16) {
    p[0] = (v >> 8) as u8;
    p[1] = v as u8;
}

#[inline]
fn w32(p: &mut [u8], v: u32) {
    w16(p, (v >> 16) as u16);
    w16(&mut p[2..], v as u16);
}

fn csum(data: &[u8], extra: u32) -> u16 {
    let mut sum = extra as u32;
    let mut i = 0;
    let mut n = data.len();
    while n >= 2 {
        sum += b16(&data[i..]) as u32;
        i += 2;
        n -= 2;
    }
    if n > 0 {
        sum += (data[i] as u32) << 8;
    }
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    !(sum as u16)
}

fn pseudo(src: &[u8], dst: &[u8], proto: u8, tlen: u16) -> u16 {
    let mut sum: u32 = 0;
    for i in 0..2 {
        sum += (src[i * 2] as u32) << 8 | src[i * 2 + 1] as u32;
    }
    for i in 0..2 {
        sum += (dst[i * 2] as u32) << 8 | dst[i * 2 + 1] as u32;
    }
    sum += proto as u32;
    sum += tlen as u32;
    while sum >> 16 != 0 {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    sum as u16
}

/* ---- constants ---- */

pub const NS_OK: i32 = 0;
pub const NS_ERR_NONET: i32 = 1;
pub const NS_ERR_DNS: i32 = 2;
pub const NS_ERR_CONN: i32 = 3;
pub const NS_ERR_HTTP: i32 = 4;
pub const NS_ERR_ABORT: i32 = 5;

const ETHERTYPE_IPV4: u16 = 0x0800;
const ETHERTYPE_ARP: u16 = 0x0806;
const ETH_HLEN: usize = 14;
const IPV4_HLEN: usize = 20;
const UDP_HLEN: usize = 8;
const TCP_HLEN: usize = 20;
const ICMP_ECHO_REQ: u8 = 8;
const ICMP_ECHO_REPLY: u8 = 0;

const TCP_FIN: u8 = 0x01;
const TCP_SYN: u8 = 0x02;
const TCP_RST: u8 = 0x04;
const TCP_ACK: u8 = 0x10;

const NS_PH_DNS: i32 = 0;
const NS_PH_CONNECT: i32 = 1;
const NS_PH_TRANSFER: i32 = 2;

const TCP_S_SYN_SENT: i32 = 1;
const TCP_S_ESTAB: i32 = 2;

const NS_MAX_HOST: usize = 96;
const NS_MAX_PATH: usize = 256;
const NS_REQ_MAX: usize = 512;
const NS_HDR_MAX: usize = 2048;
const NS_RETRY: u64 = 50;
const NS_DEADLINE: u64 = 1500;

const GW_IP: [u8; 4] = [10, 0, 2, 2];
const DNS_IP: [u8; 4] = [10, 0, 2, 3];
const MY_IP: [u8; 4] = [10, 0, 2, 15];
const BCAST_MAC: [u8; 6] = [0xFF; 6];

/* ---- ARP cache ---- */

#[derive(Clone, Copy)]
struct ArpEnt {
    ip: u32,
    mac: [u8; 6],
    valid: bool,
}

const NS_ARP_CACHE: usize = 8;
static mut ARP: [ArpEnt; NS_ARP_CACHE] = [ArpEnt { ip: 0, mac: [0; 6], valid: false }; NS_ARP_CACHE];

fn ip4(p: &[u8]) -> u32 {
    ((p[0] as u32) << 24) | ((p[1] as u32) << 16) | ((p[2] as u32) << 8) | p[3] as u32
}

fn arp_lookup(ip: &[u8]) -> Option<[u8; 6]> {
    let target = ip4(ip);
    unsafe {
        for i in 0..NS_ARP_CACHE {
            if ARP[i].valid && ARP[i].ip == target {
                return Some(ARP[i].mac);
            }
        }
    }
    None
}

fn arp_update(ip: &[u8], mac: &[u8]) {
    let target = ip4(ip);
    unsafe {
        for i in 0..NS_ARP_CACHE {
            if ARP[i].valid && ARP[i].ip == target {
                ARP[i].mac.copy_from_slice(mac);
                return;
            }
        }
        for i in 0..NS_ARP_CACHE {
            if !ARP[i].valid {
                ARP[i].ip = target;
                ARP[i].mac.copy_from_slice(mac);
                ARP[i].valid = true;
                return;
            }
        }
        ARP[0].ip = target;
        ARP[0].mac.copy_from_slice(mac);
    }
}

fn arp_request(ip: &[u8]) {
    let mut f = [0u8; ETH_HLEN + 28];
    f[0..6].copy_from_slice(&BCAST_MAC);
    net::mac().copy_from_slice(&mut f[6..12]);
    w16(&mut f[12..], ETHERTYPE_ARP);
    let a = ETH_HLEN;
    w16(&mut f[a..], 0x0001);
    w16(&mut f[a + 2..], ETHERTYPE_IPV4);
    f[a + 4] = 6;
    f[a + 5] = 4;
    w16(&mut f[a + 6..], 1);
    net::mac().copy_from_slice(&mut f[a + 8..a + 14]);
    f[a + 14..a + 18].copy_from_slice(&MY_IP);
    f[a + 18..a + 24].fill(0);
    f[a + 24..a + 28].copy_from_slice(ip);
    let _ = net::send(&f);
}

/* ---- outgoing frame builders ---- */

static mut IP_ID: u16 = 0;

fn ip_send(proto: u8, dst_ip: &[u8], dst_mac: &[u8], pay: &[u8]) {
    if pay.len() > 1500 {
        return;
    }
    let mut f = [0u8; ETH_HLEN + IPV4_HLEN + 1500];
    f[0..6].copy_from_slice(dst_mac);
    net::mac().copy_from_slice(&mut f[6..12]);
    w16(&mut f[12..], ETHERTYPE_IPV4);
    let ip = &mut f[ETH_HLEN..];
    ip[0] = 0x45;
    ip[1] = 0;
    w16(&mut ip[2..], (IPV4_HLEN + pay.len()) as u16);
    unsafe {
        w16(&mut ip[4..], IP_ID);
        IP_ID = IP_ID.wrapping_add(1);
    }
    w16(&mut ip[6..], 0);
    ip[8] = 64;
    ip[9] = proto;
    ip[12..16].copy_from_slice(&MY_IP);
    ip[16..20].copy_from_slice(dst_ip);
    let cksum = csum(&ip[..IPV4_HLEN], 0);
    w16(&mut ip[10..], cksum);
    f[ETH_HLEN + IPV4_HLEN..ETH_HLEN + IPV4_HLEN + pay.len()].copy_from_slice(pay);
    let _ = net::send(&f[..ETH_HLEN + IPV4_HLEN + pay.len()]);
}

fn udp_send(sport: u16, dport: u16, dst_ip: &[u8], dst_mac: &[u8], pay: &[u8]) {
    if pay.len() > 1500 {
        return;
    }
    let mut u = [0u8; UDP_HLEN + 1500];
    w16(&mut u[..2], sport);
    w16(&mut u[2..4], dport);
    w16(&mut u[4..6], (UDP_HLEN + pay.len()) as u16);
    w16(&mut u[6..8], 0);
    if !pay.is_empty() {
        u[UDP_HLEN..UDP_HLEN + pay.len()].copy_from_slice(pay);
    }
    let ps = pseudo(&MY_IP, dst_ip, 0x11, (UDP_HLEN + pay.len()) as u16);
    let cksum = csum(&u[..UDP_HLEN + pay.len()], ps as u32);
    w16(&mut u[6..8], cksum);
    ip_send(0x11, dst_ip, dst_mac, &u[..UDP_HLEN + pay.len()]);
}

/* ---- connection state ---- */

struct Conn {
    active: bool,
    phase: i32,
    tcp_state: i32,

    host: [u8; NS_MAX_HOST],
    path: [u8; NS_MAX_PATH],
    buf: *mut u8,
    cap: usize,
    cb: Option<fn(ctx: *mut (), status: i32, off: usize, len: usize)>,
    ctx: *mut (),

    dns_id: u16,
    dns_sport: u16,
    dns_sent: bool,
    dst_ip: [u8; 4],

    sport: u16,
    isn: u32,
    snd_nxt: u32,
    rcv_nxt: u32,
    req_sent: bool,
    req_acked: bool,
    req_len: usize,

    hdr_done: bool,
    hdr_end: usize,
    http_status: i32,
    got: usize,

    last_send: u64,
    deadline: u64,
    retries: i32,
}

impl Conn {
    fn reset(&mut self) {
        *self = Conn {
            active: false,
            phase: 0,
            tcp_state: 0,
            host: [0; NS_MAX_HOST],
            path: [0; NS_MAX_PATH],
            buf: core::ptr::null_mut(),
            cap: 0,
            cb: None,
            ctx: core::ptr::null_mut(),
            dns_id: 0,
            dns_sport: 0,
            dns_sent: false,
            dst_ip: [0; 4],
            sport: 0,
            isn: 0,
            snd_nxt: 0,
            rcv_nxt: 0,
            req_sent: false,
            req_acked: false,
            req_len: 0,
            hdr_done: false,
            hdr_end: 0,
            http_status: 0,
            got: 0,
            last_send: 0,
            deadline: 0,
            retries: 0,
        };
    }
}

static mut CONN: Conn = unsafe { core::mem::zeroed() };
static mut PORT_CTR: u16 = 0;

/* ---- TCP emit ---- */

fn tcp_emit(flags: u8, seq: u32, ack: u32, pay: &[u8]) {
    if pay.len() > 1500 {
        return;
    }
    let mut t = [0u8; TCP_HLEN + 1500];
    {
        let h = &mut t[..TCP_HLEN];
        w16(&mut h[..2], unsafe { CONN.sport });
        w16(&mut h[2..4], 80);
        w32(&mut h[4..8], seq);
        w32(&mut h[8..12], ack);
        h[12] = (TCP_HLEN / 4) as u8;
        h[13] = flags;
        w16(&mut h[14..16], 65535);
        w16(&mut h[16..18], 0);
        w16(&mut h[18..20], 0);
    }
    if !pay.is_empty() {
        t[TCP_HLEN..TCP_HLEN + pay.len()].copy_from_slice(pay);
    }
    let ps = pseudo(&MY_IP, unsafe { &CONN.dst_ip }, 0x06, (TCP_HLEN + pay.len()) as u16);
    let cksum = csum(&t[..TCP_HLEN + pay.len()], ps as u32);
    {
        let h = &mut t[..TCP_HLEN];
        w16(&mut h[16..18], cksum);
    }
    ip_send(0x06, unsafe { &CONN.dst_ip }, &BCAST_MAC, &t[..TCP_HLEN + pay.len()]);
}

fn finish(status: i32) {
    let cb = unsafe { CONN.cb };
    let ctx = unsafe { CONN.ctx };
    let mut off = 0;
    let mut len = 0;
    if status == NS_OK && unsafe { CONN.hdr_done } && unsafe { CONN.hdr_end } <= unsafe { CONN.got } {
        off = unsafe { CONN.hdr_end };
        len = unsafe { CONN.got } - off;
    }
    unsafe {
        CONN.reset();
    }
    if let Some(f) = cb {
        f(ctx, status, off, len);
    }
}

/* ---- DNS ---- */

fn dns_build_name(dst: &mut [u8], name: &[u8]) {
    let mut off = 0;
    let mut i = 0;
    while i < name.len() && off + 1 < dst.len() {
        let mut part = 0;
        while i + part < name.len() && name[i + part] != b'.' && part < 63 {
            part += 1;
        }
        if part == 0 || off + 1 + part >= dst.len() {
            break;
        }
        dst[off] = part as u8;
        off += 1;
        dst[off..off + part].copy_from_slice(&name[i..i + part]);
        off += part;
        i += part;
        if i < name.len() && name[i] == b'.' {
            i += 1;
        } else {
            break;
        }
    }
    if off < dst.len() {
        dst[off] = 0;
    }
}

fn dns_send_query() {
    let mut q = [0u8; 512];
    unsafe {
        w16(&mut q[..2], CONN.dns_id);
        w16(&mut q[2..4], 0x0100);
        w16(&mut q[4..6], 1);
    }
    dns_build_name(&mut q[12..], &unsafe { CONN.host });
    let qend = 12 + unsafe { CONN.host }.iter().position(|&b| b == 0).unwrap_or(NS_MAX_HOST) + 1;
    w16(&mut q[qend..qend + 2], 1);
    w16(&mut q[qend + 2..qend + 4], 1);
    let _ = udp_send(unsafe { CONN.dns_sport }, 53, &DNS_IP, &BCAST_MAC, &q[..qend + 4]);
    unsafe {
        CONN.last_send = timer::ticks();
    }
}

fn dns_skip_name(p: &[u8], mut off: usize) -> Option<usize> {
    while off < p.len() {
        let len = p[off];
        if len == 0 {
            return Some(off + 1);
        }
        if (len & 0xC0) == 0xC0 {
            return Some(off + 2);
        }
        if len > 63 || off + 1 + len as usize > p.len() {
            return None;
        }
        off += 1 + len as usize;
    }
    None
}

fn dns_handle(q: &[u8]) {
    if q.len() < 12 {
        return;
    }
    unsafe {
        if b16(&q[..2]) != CONN.dns_id {
            return;
        }
    }
    if b16(&q[2..4]) & 0x8000 == 0 {
        return;
    }
    if b16(&q[2..4]) & 0x000F != 0 {
        finish(NS_ERR_DNS);
        return;
    }
    let qd = b16(&q[4..6]) as usize;
    let an = b16(&q[6..8]) as usize;
    let mut off = 12;
    for _ in 0..qd {
        off = match dns_skip_name(q, off) {
            Some(o) => o,
            None => return,
        };
        off += 4;
    }
    for _ in 0..an {
        off = match dns_skip_name(q, off) {
            Some(o) => o,
            None => return,
        };
        if off + 10 > q.len() {
            return;
        }
        let typ = b16(&q[off..]);
        let cls = b16(&q[off + 2..]);
        let rd = b16(&q[off + 8..]) as usize;
        if typ == 1 && cls == 1 && rd == 4 && off + 14 <= q.len() {
            unsafe {
                CONN.dst_ip[0] = q[off + 10];
                CONN.dst_ip[1] = q[off + 11];
                CONN.dst_ip[2] = q[off + 12];
                CONN.dst_ip[3] = q[off + 13];
            }
            unsafe {
                CONN.phase = NS_PH_CONNECT;
            }
            return;
        }
        off += 10 + rd;
        if off > q.len() {
            return;
        }
    }
    finish(NS_ERR_DNS);
}

/* ---- TCP ---- */

fn tcp_handle(seg: &[u8]) {
    if seg.len() < TCP_HLEN {
        return;
    }
    let flags = seg[13] & 0x3F;
    let seq = b32(&seg[4..]);
    let ack = b32(&seg[8..]);
    let doff = ((seg[12] >> 4) * 4) as usize;
    if doff < TCP_HLEN || doff > seg.len() {
        return;
    }
    let dlen = seg.len() - doff;
    let dp = &seg[doff..];

    unsafe {
        if CONN.tcp_state == TCP_S_SYN_SENT {
            if !(flags & TCP_SYN != 0 && flags & TCP_ACK != 0) {
                return;
            }
            if ack != CONN.isn + 1 {
                return;
            }
            if flags & TCP_RST != 0 {
                finish(NS_ERR_CONN);
                return;
            }
            CONN.rcv_nxt = seq + 1;
            CONN.tcp_state = TCP_S_ESTAB;
            CONN.phase = NS_PH_TRANSFER;
            send_request();
            return;
        }

        if CONN.tcp_state != TCP_S_ESTAB {
            return;
        }

        if flags & TCP_RST != 0 {
            finish(NS_ERR_CONN);
            return;
        }

        if (flags & TCP_ACK != 0) && CONN.req_sent && !CONN.req_acked && ack >= CONN.snd_nxt {
            CONN.req_acked = true;
        }

        let mut adv = 0;
        if dlen > 0 && seq == CONN.rcv_nxt {
            let room = CONN.cap - CONN.got;
            let take = if dlen > room { room } else { dlen };
            if take > 0 {
                let dst = core::slice::from_raw_parts_mut(CONN.buf.add(CONN.got), take);
                dst.copy_from_slice(&dp[..take]);
                CONN.got += take;
            }
            CONN.rcv_nxt += take as u32;
            adv = take;

            if !CONN.hdr_done {
                let scan = if CONN.got < NS_HDR_MAX { CONN.got } else { NS_HDR_MAX };
                for i in 3..scan {
                    if CONN.buf.add(i - 3).read_volatile() == b'\r'
                        && CONN.buf.add(i - 2).read_volatile() == b'\n'
                        && CONN.buf.add(i - 1).read_volatile() == b'\r'
                        && CONN.buf.add(i).read_volatile() == b'\n'
                    {
                        CONN.hdr_done = true;
                        CONN.hdr_end = i + 1;
                        if CONN.hdr_end >= 12
                            && CONN.buf.add(0).read_volatile() == b'H'
                            && CONN.buf.add(1).read_volatile() == b'T'
                            && CONN.buf.add(2).read_volatile() == b'T'
                            && CONN.buf.add(3).read_volatile() == b'P'
                        {
                            let c1 = CONN.buf.add(9).read_volatile();
                            let c2 = CONN.buf.add(10).read_volatile();
                            let c3 = CONN.buf.add(11).read_volatile();
                            if (b'0'..=b'9').contains(&c1)
                                && (b'0'..=b'9').contains(&c2)
                                && (b'0'..=b'9').contains(&c3)
                            {
                                CONN.http_status =
                                    (c1 - b'0') as i32 * 100 + (c2 - b'0') as i32 * 10 + (c3 - b'0') as i32;
                            }
                        }
                        break;
                    }
                }
            }

            if take < dlen {
                tcp_emit(TCP_ACK, CONN.snd_nxt, CONN.rcv_nxt, &[]);
                finish(if CONN.http_status >= 200 && CONN.http_status < 400 {
                    NS_OK
                } else {
                    NS_ERR_HTTP
                });
                return;
            }
        } else if dlen > 0 {
            tcp_emit(TCP_ACK, CONN.snd_nxt, CONN.rcv_nxt, &[]);
            return;
        }

        if flags & TCP_FIN != 0 {
            CONN.rcv_nxt += 1;
            tcp_emit(TCP_ACK | TCP_FIN, CONN.snd_nxt, CONN.rcv_nxt, &[]);
            finish(if CONN.http_status >= 200 && CONN.http_status < 400 {
                NS_OK
            } else {
                NS_ERR_HTTP
            });
            return;
        }

        if adv > 0 {
            tcp_emit(TCP_ACK, CONN.snd_nxt, CONN.rcv_nxt, &[]);
        }
    }
}

fn send_request() {
    unsafe {
        if CONN.req_len == 0 {
            let req = b"GET ";
            let path = &CONN.path[..CONN.path.len().min(NS_MAX_PATH)];
            let host = &CONN.host[..CONN.host.len().min(NS_MAX_HOST)];
            let path_str = core::str::from_utf8(path).unwrap_or("/");
            let host_str = core::str::from_utf8(host).unwrap_or("");
            let path_bytes = path_str.as_bytes();
            let host_bytes = host_str.as_bytes();
            let req_parts: [&[u8]; 5] = [
                req,
                path_bytes,
                b" HTTP/1.0\r\nHost: ",
                host_bytes,
                b"\r\nUser-Agent: SolOS/0.1\r\nConnection: close\r\n\r\n",
            ];
            let mut w = 0;
            for part in &req_parts {
                for &b in part.iter() {
                    if w < NS_REQ_MAX - 1 {
                        CONN.buf.add(w).write_volatile(b);
                        w += 1;
                    }
                }
            }
            CONN.buf.add(w).write_volatile(0);
            CONN.req_len = w;
        }
        let req = core::slice::from_raw_parts(CONN.buf, CONN.req_len);
        tcp_emit(TCP_ACK, CONN.snd_nxt, CONN.rcv_nxt, req);
        CONN.snd_nxt += CONN.req_len as u32;
        CONN.req_sent = true;
        CONN.last_send = timer::ticks();
    }
}

/* ---- RX dispatch ---- */

fn rx_ipv4(p: &[u8]) {
    if p.len() < IPV4_HLEN {
        return;
    }
    let ihl = ((p[0] & 0x0F) * 4) as usize;
    if ihl < IPV4_HLEN || ihl > p.len() {
        return;
    }
    let proto = p[9];
    let tlen = b16(&p[2..]) as usize;
    let plen = if tlen < ihl { 0 } else { (tlen - ihl).min(p.len() - ihl) };
    let pay = &p[ihl..ihl + plen];

    if proto == 0x11 && plen >= UDP_HLEN {
        let dport = b16(&pay[2..]);
        unsafe {
            if dport == CONN.dns_sport && CONN.phase == NS_PH_DNS {
                dns_handle(&pay[UDP_HLEN..]);
            }
        }
    } else if proto == 0x06 && plen >= TCP_HLEN {
        let dport = b16(&pay[2..]);
        unsafe {
            if CONN.active && dport == CONN.sport {
                tcp_handle(pay);
            }
        }
    }
}

fn rx_frame(f: &[u8]) {
    if f.len() < ETH_HLEN {
        return;
    }
    let typ = b16(&f[12..]);
    let p = &f[ETH_HLEN..];
    let plen = f.len() - ETH_HLEN;

    if typ == ETHERTYPE_IPV4 {
        rx_ipv4(p);
    } else if typ == ETHERTYPE_ARP && plen >= 28 {
        if b16(&p[6..]) == 2 {
            arp_update(&p[14..], &p[8..14]);
        }
    }
}

/* ---- public API ---- */

pub type NsCallback = fn(ctx: *mut (), status: i32, off: usize, len: usize);

/// Initiates an HTTP GET. Returns 0 on success, -1 if no NIC/link.
pub fn http_get(host: &[u8], path: &[u8], buf: *mut u8, cap: usize, cb: Option<NsCallback>, ctx: *mut ()) -> i32 {
    if !net::present() || !net::link_up() {
        if let Some(f) = cb {
            f(ctx, NS_ERR_NONET, 0, 0);
        }
        return -1;
    }

    unsafe {
        CONN.reset();
        CONN.active = true;
        CONN.phase = NS_PH_DNS;
        CONN.buf = buf;
        CONN.cap = cap;
        CONN.cb = cb;
        CONN.ctx = ctx;

        let mut i = 0;
        while i < host.len() && i + 1 < NS_MAX_HOST {
            CONN.host[i] = host[i];
            i += 1;
        }
        CONN.host[i] = 0;

        let mut j = 0;
        while j < path.len() && j + 1 < NS_MAX_PATH {
            CONN.path[j] = path[j];
            j += 1;
        }
        CONN.path[j] = 0;

        unsafe {
            PORT_CTR += 1;
            CONN.sport = 40000 + (PORT_CTR % 15000);
            CONN.dns_sport = 50000 + (PORT_CTR % 10000);
            CONN.dns_id = 0x1000 + (PORT_CTR % 0x7000);
            CONN.isn = 0x12345678u32.wrapping_add(timer::ticks() as u32 * 97).wrapping_add(PORT_CTR as u32 * 7919);
            CONN.snd_nxt = CONN.isn + 1;
            CONN.deadline = timer::ticks() + NS_DEADLINE;
        }
        net::mac().copy_from_slice(&mut CONN.dst_ip); // temp placeholder, will be set by DNS
    }
    0
}

pub fn abort(ctx: *mut ()) {
    unsafe {
        if CONN.active && CONN.ctx == ctx {
            CONN.reset();
        }
    }
}

pub fn poll() {
    if !net::present() {
        return;
    }

    net::poll();

    unsafe {
        if !CONN.active {
            return;
        }
        let now = timer::ticks();
        if now >= CONN.deadline {
            finish(NS_ERR_CONN);
            return;
        }

        if CONN.phase == NS_PH_DNS {
            if !CONN.dns_sent {
                if arp_lookup(&DNS_IP).is_some() {
                    dns_send_query();
                    CONN.dns_sent = true;
                } else if now - CONN.last_send >= NS_RETRY {
                    arp_request(&DNS_IP);
                    CONN.last_send = now;
                }
            } else if now - CONN.last_send >= NS_RETRY {
                if CONN.retries > 8 {
                    finish(NS_ERR_DNS);
                    return;
                }
                CONN.retries += 1;
                dns_send_query();
            }
            return;
        }

        if CONN.phase == NS_PH_CONNECT {
            if arp_lookup(&GW_IP).is_none() {
                if now - CONN.last_send >= NS_RETRY {
                    arp_request(&GW_IP);
                    CONN.last_send = now;
                }
                return;
            }
            if CONN.tcp_state != TCP_S_SYN_SENT {
                CONN.tcp_state = TCP_S_SYN_SENT;
                tcp_emit(TCP_SYN, CONN.isn, 0, &[]);
                CONN.last_send = now;
            } else if now - CONN.last_send >= NS_RETRY {
                if CONN.retries > 24 {
                    finish(NS_ERR_CONN);
                    return;
                }
                CONN.retries += 1;
                tcp_emit(TCP_SYN, CONN.isn, 0, &[]);
            }
            return;
        }

        /* NS_PH_TRANSFER: retransmit request if nothing acked */
        if CONN.req_sent && !CONN.req_acked && now - CONN.last_send >= NS_RETRY {
            if CONN.retries > 10 {
                finish(NS_ERR_CONN);
                return;
            }
            CONN.retries += 1;
            let req = core::slice::from_raw_parts(CONN.buf, CONN.req_len);
            tcp_emit(TCP_ACK, CONN.snd_nxt - CONN.req_len as u32, CONN.rcv_nxt, req);
            CONN.last_send = now;
        }
    }
}
