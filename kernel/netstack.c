#include "netstack.h"
#include "net.h"
#include "tls.h"
#include "klog.h"
#include "arch/x86_64/timer.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- byte helpers ---- */

static inline uint16_t b16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t b32(const uint8_t *p) {
    return ((uint32_t)b16(p) << 16) | b16(p + 2);
}
static inline void w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void w32(uint8_t *p, uint32_t v) {
    w16(p, (uint16_t)(v >> 16));
    w16(p + 2, (uint16_t)v);
}

/* Internet checksum: `extra` carries the folded sum of the TCP/UDP
 * pseudo-header so the same routine serves IP and transport. */
static uint16_t csum(const void *data, size_t len, uint32_t extra) {
    const uint8_t *b = (const uint8_t *)data;
    uint32_t sum = extra;
    while (len >= 2) {
        sum += b16(b);
        b += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)b[0] << 8;
    while (sum >> 16) sum = (uint16_t)sum + (uint16_t)(sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t pseudo(const uint8_t *src, const uint8_t *dst,
                       uint8_t proto, uint16_t tlen) {
    uint32_t sum = ((uint32_t)src[0] << 8 | src[1]) +
                   ((uint32_t)src[2] << 8 | src[3]) +
                   ((uint32_t)dst[0] << 8 | dst[1]) +
                   ((uint32_t)dst[2] << 8 | dst[3]) +
                   (uint32_t)proto + tlen;
    while (sum >> 16) sum = (uint16_t)sum + (uint16_t)(sum >> 16);
    return (uint16_t)sum;
}

/* ---- addresses ---- */

static const uint8_t ns_my_ip[4]  = { 10, 0, 2, 15 };
static const uint8_t ns_gw_ip[4]  = { 10, 0, 2, 2 };
static const uint8_t ns_dns_ip[4] = { 10, 0, 2, 3 };
static const uint8_t ns_bcast[6]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t ns_my_mac[6];

#define ETHERTYPE_IPV4  0x0800u
#define ETHERTYPE_ARP   0x0806u
#define ETH_HLEN        14u
#define IPV4_HLEN       20u
#define TCP_HLEN        20u
#define UDP_HLEN        8u

#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u

/* ---- ARP cache ---- */

struct arp_ent { uint32_t ip; uint8_t mac[6]; int valid; };
#define NS_ARP_CACHE 8
static struct arp_ent ns_arp[NS_ARP_CACHE];

static uint32_t ip4(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static const uint8_t *arp_lookup(const uint8_t *ip) {
    for (int i = 0; i < NS_ARP_CACHE; i++)
        if (ns_arp[i].valid && ns_arp[i].ip == ip4(ip)) return ns_arp[i].mac;
    return NULL;
}

static void arp_update(const uint8_t *ip, const uint8_t *mac) {
    for (int i = 0; i < NS_ARP_CACHE; i++) {
        if (ns_arp[i].valid && ns_arp[i].ip == ip4(ip)) {
            memcpy(ns_arp[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < NS_ARP_CACHE; i++) {
        if (!ns_arp[i].valid) {
            ns_arp[i].ip = ip4(ip);
            memcpy(ns_arp[i].mac, mac, 6);
            ns_arp[i].valid = 1;
            return;
        }
    }
    ns_arp[0].ip = ip4(ip);
    memcpy(ns_arp[0].mac, mac, 6);
}

static void arp_request(const uint8_t *ip) {
    uint8_t f[ETH_HLEN + 28];
    memcpy(f + 0, ns_bcast, 6);
    memcpy(f + 6, ns_my_mac, 6);
    w16(f + 12, ETHERTYPE_ARP);
    uint8_t *a = f + ETH_HLEN;
    w16(a + 0, 0x0001);             /* htype: ethernet */
    w16(a + 2, ETHERTYPE_IPV4);     /* ptype */
    a[4] = 6; a[5] = 4;             /* hlen, plen */
    w16(a + 6, 1);                  /* op: REQUEST */
    memcpy(a + 8, ns_my_mac, 6);    /* sha */
    memcpy(a + 14, ns_my_ip, 4);    /* spa */
    memset(a + 18, 0, 6);           /* tha */
    memcpy(a + 24, ip, 4);          /* tpa */
    net_send(f, sizeof(f));
}

/* ---- outgoing frame builders ---- */

static uint16_t g_ip_id;

static void ip_send(uint8_t proto, const uint8_t *dst_ip,
                    const uint8_t *dst_mac, const void *pay, size_t plen) {
    uint8_t f[ETH_HLEN + IPV4_HLEN + 1500];
    if (plen > 1500) return;
    uint8_t *ip = f + ETH_HLEN;
    ip[0] = 0x45;
    ip[1] = 0;
    w16(ip + 2, (uint16_t)(IPV4_HLEN + plen));
    w16(ip + 4, ++g_ip_id);
    w16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = proto;
    w16(ip + 10, 0);                /* checksum filled below */
    memcpy(ip + 12, ns_my_ip, 4);
    memcpy(ip + 16, dst_ip, 4);
    w16(ip + 10, csum(ip, IPV4_HLEN, 0));
    memcpy(ip + IPV4_HLEN, pay, plen);

    memcpy(f + 0, dst_mac, 6);
    memcpy(f + 6, ns_my_mac, 6);
    w16(f + 12, ETHERTYPE_IPV4);
    net_send(f, ETH_HLEN + IPV4_HLEN + plen);
}

static void udp_send(uint16_t sport, uint16_t dport,
                     const uint8_t *dst_ip, const uint8_t *dst_mac,
                     const void *pay, size_t plen) {
    uint8_t u[UDP_HLEN + 1500];
    if (plen > 1500) return;
    uint8_t *h = u;
    w16(h + 0, sport);
    w16(h + 2, dport);
    w16(h + 4, (uint16_t)(UDP_HLEN + plen));
    w16(h + 6, 0);
    if (plen) memcpy(h + UDP_HLEN, pay, plen);
    uint16_t ps = pseudo(ns_my_ip, dst_ip, 0x11, (uint16_t)(UDP_HLEN + plen));
    w16(h + 6, csum(h, UDP_HLEN + plen, ps));
    ip_send(0x11, dst_ip, dst_mac, u, UDP_HLEN + plen);
}

/* ---- single TCP client connection ----
 *
 * The PIT ticks at 100 Hz, so every tick is 10 ms. All the timeout
 * constants below are expressed in ticks. */

#define NS_PH_DNS      0
#define NS_PH_CONNECT  1
#define NS_PH_TLS      2
#define NS_PH_TRANSFER 3

#define TCP_S_SYN_SENT  1
#define TCP_S_ESTAB     2

#define NS_MAX_HOST    96
#define NS_MAX_PATH    256
#define NS_REQ_MAX     512
#define NS_HDR_MAX     2048
#define NS_RETRY       50       /* 0.5 s between retransmits */
#define NS_DEADLINE    1500     /* 15 s for the whole request */

struct ns_conn_t {
    int active;
    int phase;
    int tcp_state;

    char host[NS_MAX_HOST];
    char path[NS_MAX_PATH];
    char *buf;
    size_t cap;
    ns_cb_t cb;
    void *ctx;

    int use_tls;
    int tls_sent;

    /* DNS */
    uint16_t dns_id;
    uint16_t dns_sport;
    int dns_sent;
    uint8_t dst_ip[4];

    /* TCP */
    uint16_t sport;
    uint32_t isn;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    int req_sent;
    int req_acked;
    size_t req_len;
    char req[NS_REQ_MAX];

    /* body handling */
    int hdr_done;
    size_t hdr_end;          /* index just past the "\r\n\r\n" when found */
    int http_status;
    size_t got;              /* total bytes written into buf */

    /* timers (ticks @ 100 Hz) */
    uint64_t last_send;
    uint64_t deadline;
    int retries;
};
static struct ns_conn_t ns_conn;
static uint16_t ns_port_ctr;

/* Emits a TCP segment on the active connection (dst port 80 for HTTP,
 * 443 for HTTPS; routed through the gateway MAC) with IP/TCP checksums. */
static void ns_emit(uint8_t flags, uint32_t seq, uint32_t ack,
                    const void *pay, size_t plen) {
    uint8_t t[TCP_HLEN + 1500];
    if (plen > 1500) return;
    uint8_t *h = t;
    w16(h + 0, ns_conn.sport);
    w16(h + 2, ns_conn.use_tls ? 443 : 80);
    w32(h + 4, seq);
    w32(h + 8, ack);
    h[12] = (uint8_t)((TCP_HLEN / 4) << 4);
    h[13] = (uint8_t)flags;
    w16(h + 14, 65535);             /* window */
    w16(h + 16, 0);                 /* checksum filled below */
    w16(h + 18, 0);                 /* urgent */
    if (plen) memcpy(h + TCP_HLEN, pay, plen);
    uint16_t ps = pseudo(ns_my_ip, ns_conn.dst_ip, 0x06,
                         (uint16_t)(TCP_HLEN + plen));
    w16(h + 16, csum(h, TCP_HLEN + plen, ps));
    ip_send(0x06, ns_conn.dst_ip, arp_lookup(ns_gw_ip), t, TCP_HLEN + plen);
}

/* Send raw TCP payload (used by TLS layer). */
void ns_tls_send(const void *data, size_t len) {
    if (!ns_conn.active || ns_conn.tcp_state != TCP_S_ESTAB) return;
    if (len > 1500) len = 1500;
    ns_emit(TCP_PSH | TCP_ACK, ns_conn.snd_nxt, ns_conn.rcv_nxt, data, len);
    ns_conn.snd_nxt += (uint32_t)len;
    ns_conn.last_send = timer_get_ticks();
}

static void ns_finish(int status) {
    ns_cb_t cb = ns_conn.cb;
    void *ctx = ns_conn.ctx;
    size_t off = 0, len = 0;
    if (status == NS_OK && ns_conn.hdr_done && ns_conn.hdr_end <= ns_conn.got) {
        off = ns_conn.hdr_end;
        len = ns_conn.got - off;
    }
    memset(&ns_conn, 0, sizeof ns_conn);
    if (cb) cb(ctx, status, off, len);
}

/* ---- DNS ---- */

static void dns_build_name(uint8_t *dst, size_t cap, const char *name) {
    size_t off = 0;
    while (*name && off < cap) {
        size_t part = 0;
        while (name[part] && name[part] != '.' && part < 64) part++;
        if (part > 63 || off + part + 1 >= cap) break;
        dst[off++] = (uint8_t)part;
        memcpy(dst + off, name, part);
        off += part;
        name += part;
        if (*name == '.') name++;
        else break;
    }
    if (off < cap) dst[off] = 0;
}

static void dns_send_query(void) {
    uint8_t q[512];
    memset(q, 0, sizeof q);
    w16(q + 0, ns_conn.dns_id);
    w16(q + 2, 0x0100);             /* RD */
    w16(q + 4, 1);                  /* qdcount */
    dns_build_name(q + 12, sizeof(q) - 12, ns_conn.host);
    size_t qname_len = strlen((char *)q + 12) + 1;
    size_t qend = 12 + qname_len;
    w16(q + qend, 1);               /* qtype: A */
    w16(q + qend + 2, 1);           /* qclass: IN */
    udp_send(ns_conn.dns_sport, 53, ns_dns_ip, arp_lookup(ns_dns_ip),
             q, qend + 4);
    ns_conn.last_send = timer_get_ticks();
}

/* Skips a possibly-compressed DNS name; returns the new offset or 0. */
static size_t dns_skip_name(const uint8_t *p, size_t n, size_t off) {
    uint8_t len = p[off];
    if (len == 0) return off + 1;
    if ((len & 0xC0) == 0xC0) return off + 2;
    if ((len & 0xC0) != 0) return 0;
    if (off + 1 + len >= n) return 0;
    return dns_skip_name(p, n, off + 1 + len);
}

static void dns_handle(const uint8_t *q, size_t n) {
    if (n < 12) return;
    if (b16(q + 0) != ns_conn.dns_id) return;
    uint16_t flags = b16(q + 2);
    if (!(flags & 0x8000)) return;              /* not a response */
    if ((flags & 0x000F) != 0) {                /* rcode != 0 */
        klog("[net] dns rcode=%u for %s\n", flags & 0x000F, ns_conn.host);
        ns_finish(NS_ERR_DNS);
        return;
    }
    uint16_t qd = b16(q + 4);
    uint16_t an = b16(q + 6);
    size_t off = 12;
    for (uint16_t i = 0; i < qd; i++) {
        off = dns_skip_name(q, n, off);
        if (off == 0) return;
        off += 4;                               /* qtype + qclass */
        if (off >= n) return;
    }
    for (uint16_t i = 0; i < an; i++) {
        off = dns_skip_name(q, n, off);
        if (off == 0) return;
        if (off + 10 > n) return;
        uint16_t type = b16(q + off);
        uint16_t cls  = b16(q + off + 2);
        uint16_t rd   = b16(q + off + 8);
        if (type == 1 && cls == 1 && rd == 4 && off + 14 <= n) {
            ns_conn.dst_ip[0] = q[off + 10];
            ns_conn.dst_ip[1] = q[off + 11];
            ns_conn.dst_ip[2] = q[off + 12];
            ns_conn.dst_ip[3] = q[off + 13];
            klog("[net] dns ok %s = %u.%u.%u.%u\n",
                 ns_conn.host, ns_conn.dst_ip[0], ns_conn.dst_ip[1],
                 ns_conn.dst_ip[2], ns_conn.dst_ip[3]);
            ns_conn.phase = NS_PH_CONNECT;
            return;
        }
        off += 10 + rd;
        if (off > n) return;
    }
    klog("[net] dns no A record for %s\n", ns_conn.host);
    ns_finish(NS_ERR_DNS);
}

/* ---- TCP send: handshake + request ---- */

static void ns_send_request(void) {
    if (ns_conn.req_len == 0) {
        static const char *const parts[] = {
            "GET ", ns_conn.path, " HTTP/1.0\r\nHost: ",
            ns_conn.host, "\r\nUser-Agent: SolOS/0.1\r\n"
            "Connection: close\r\n\r\n",
        };
        int w = 0;
        for (int i = 0; i < 5 && w < NS_REQ_MAX - 1; i++) {
            const char *p = parts[i];
            while (*p && w < NS_REQ_MAX - 1) ns_conn.req[w++] = *p++;
        }
        ns_conn.req[w] = 0;
        ns_conn.req_len = (size_t)w;
    }
    if (ns_conn.use_tls) {
        klog("[net] sending TLS request (%u bytes)\n", (unsigned)ns_conn.req_len);
        tls_send_app_data((const uint8_t *)ns_conn.req, ns_conn.req_len);
    } else {
        ns_emit(TCP_ACK, ns_conn.snd_nxt, ns_conn.rcv_nxt,
                ns_conn.req, ns_conn.req_len);
        ns_conn.snd_nxt += (uint32_t)ns_conn.req_len;
    }
    ns_conn.req_sent = 1;
    ns_conn.last_send = timer_get_ticks();
}

/* ---- TCP receive ---- */

static void tcp_handle(const uint8_t *seg, size_t n) {
    if (n < TCP_HLEN) return;
    uint8_t flags = (uint8_t)(seg[13] & 0x3F);
    uint32_t seq = b32(seg + 4);
    uint32_t ack = b32(seg + 8);
    uint16_t doff = (uint16_t)((seg[12] >> 4) * 4);
    uint16_t ip_tlen = n;
    if (n < doff) return;
    size_t dlen = n - doff;
    const uint8_t *dp = seg + doff;
    klog("[net] tcp f=%u seq=%u ack=%u doff=%u iplen=%u dlen=%lu ",
         (unsigned)flags, (unsigned)seq, (unsigned)ack,
         (unsigned)doff, (unsigned)ip_tlen, (unsigned long)dlen);
    for (size_t i = 0; i < dlen && i < 8; i++) klog("%u ", (unsigned)dp[i]);
    klog("raw=");
    for (size_t i = 0; i < n && i < 20; i++) klog("%u ", (unsigned)seg[i]);
    klog("\n");

    if (ns_conn.tcp_state == TCP_S_SYN_SENT) {
        if (!(flags & TCP_SYN) || !(flags & TCP_ACK)) return;
        if (ack != ns_conn.isn + 1) return;
        if (flags & TCP_RST) { ns_finish(NS_ERR_CONN); return; }
        klog("[net] tcp SYN-ACK received\n");
        ns_conn.rcv_nxt = seq + 1;
        ns_conn.tcp_state = TCP_S_ESTAB;
        if (ns_conn.use_tls) {
            ns_conn.phase = NS_PH_TLS;
            klog("[net] tls handshake start %s\n", ns_conn.host);
            tls_connect(ns_conn.host, NULL, 0, NULL, NULL);
            ns_emit(TCP_ACK, ns_conn.snd_nxt, ns_conn.rcv_nxt, NULL, 0);
        } else {
            ns_conn.phase = NS_PH_TRANSFER;
            klog("[net] tcp connected to %u.%u.%u.%u\n",
                 ns_conn.dst_ip[0], ns_conn.dst_ip[1],
                 ns_conn.dst_ip[2], ns_conn.dst_ip[3]);
            ns_send_request();
        }
        return;
    }

    if (ns_conn.tcp_state != TCP_S_ESTAB) return;

    if (flags & TCP_RST) {
        klog("[net] tcp RST\n");
        ns_finish(NS_ERR_CONN);
        return;
    }

    /* Any ACK at/above the end of our request proves it was delivered. */
    if ((flags & TCP_ACK) && ns_conn.req_sent && !ns_conn.req_acked &&
        ack >= ns_conn.snd_nxt) {
        ns_conn.req_acked = 1;
    }

    size_t adv = 0;
    if (dlen && seq == ns_conn.rcv_nxt) {
        if (ns_conn.phase == NS_PH_TLS) {
            klog("[tls] feed: %u bytes, flags=%02x\n", (unsigned)dlen, flags);
            if (dlen > 0) {
                klog("[https] received %u encrypted bytes\n", (unsigned)dlen);
            }
            klog("[tls] data: ");
            for (size_t i = 0; i < dlen && i < 32; i++) {
                klog("%u ", (unsigned)dp[i]);
            }
            klog("\n");
            tls_feed(dp, dlen);
            tls_poll();
            if (tls_app_ready()) {
                klog("[https] TLS app data ready (%lu bytes)\n",
                     (unsigned long)tls_app_len());
                tls_clear_app_ready();
                size_t take = tls_app_len() < ns_conn.cap ? tls_app_len() : ns_conn.cap;
                take = tls_copy_app_data((uint8_t *)ns_conn.buf, take);
                ns_conn.got = take;
                tls_clear_app_len();
                if (!ns_conn.hdr_done && ns_conn.got >= 4) {
                    size_t scan = ns_conn.got < NS_HDR_MAX ? ns_conn.got : NS_HDR_MAX;
                    for (size_t i = 3; i < scan; i++) {
                        if (ns_conn.buf[i - 3] == '\r' && ns_conn.buf[i - 2] == '\n' &&
                            ns_conn.buf[i - 1] == '\r' && ns_conn.buf[i] == '\n') {
                            ns_conn.hdr_done = 1;
                            ns_conn.hdr_end = i + 1;
                            if (ns_conn.hdr_end >= 12 &&
                                ns_conn.buf[0] == 'H' && ns_conn.buf[1] == 'T' &&
                                ns_conn.buf[2] == 'T' && ns_conn.buf[3] == 'P') {
                                int c1 = ns_conn.buf[9];
                                int c2 = ns_conn.buf[10];
                                int c3 = ns_conn.buf[11];
                                if (c1 >= '0' && c1 <= '9' && c2 >= '0' && c2 <= '9' &&
                                    c3 >= '0' && c3 <= '9') {
                                    ns_conn.http_status = (c1 - '0') * 100 +
                                                          (c2 - '0') * 10 + (c3 - '0');
                                    klog("[https] HTTP status=%d\n", ns_conn.http_status);
                                }
                            }
                            break;
                        }
                    }
                }
            }
            ns_conn.rcv_nxt += (uint32_t)dlen;
            adv = dlen;
            ns_emit(TCP_ACK, ns_conn.snd_nxt, ns_conn.rcv_nxt, NULL, 0);
        } else {
            size_t room = ns_conn.cap - ns_conn.got;
            size_t take = dlen > room ? room : dlen;
            if (take) {
                memcpy(ns_conn.buf + ns_conn.got, dp, take);
                ns_conn.got += take;
            }
            ns_conn.rcv_nxt += (uint32_t)take;
            adv = take;

            if (!ns_conn.hdr_done) {
                size_t scan = ns_conn.got < NS_HDR_MAX ? ns_conn.got : NS_HDR_MAX;
                for (size_t i = 3; i < scan; i++) {
                    if (ns_conn.buf[i - 3] == '\r' && ns_conn.buf[i - 2] == '\n' &&
                        ns_conn.buf[i - 1] == '\r' && ns_conn.buf[i] == '\n') {
                        ns_conn.hdr_done = 1;
                        ns_conn.hdr_end = i + 1;
                        if (ns_conn.hdr_end >= 12 &&
                            ns_conn.buf[0] == 'H' && ns_conn.buf[1] == 'T' &&
                            ns_conn.buf[2] == 'T' && ns_conn.buf[3] == 'P') {
                            int c1 = ns_conn.buf[9];
                            int c2 = ns_conn.buf[10];
                            int c3 = ns_conn.buf[11];
                            if (c1 >= '0' && c1 <= '9' && c2 >= '0' && c2 <= '9' &&
                                c3 >= '0' && c3 <= '9') {
                                ns_conn.http_status = (c1 - '0') * 100 +
                                                      (c2 - '0') * 10 + (c3 - '0');
                            }
                        }
                        break;
                    }
                }
            }

            if (take < dlen) {
                /* buffer full: ack what we consumed and finish */
                ns_emit(TCP_ACK, ns_conn.snd_nxt, ns_conn.rcv_nxt, NULL, 0);
                klog("[net] http %s status=%d bytes=%lu (truncated)\n",
                     ns_conn.host, ns_conn.http_status, (unsigned long)ns_conn.got);
                ns_finish(ns_conn.http_status >= 200 && ns_conn.http_status < 400
                              ? NS_OK : NS_ERR_HTTP);
                return;
            }
        }
    } else if (dlen) {
        /* out of order: re-ACK what we already have */
        ns_emit(TCP_ACK, ns_conn.snd_nxt, ns_conn.rcv_nxt, NULL, 0);
        return;
    }

    if (flags & TCP_FIN) {
        ns_conn.rcv_nxt++;
        if (ns_conn.phase == NS_PH_TLS) {
            klog("[net] tls FIN received\n");
            int had_app = 0;
            if (tls_app_ready() || tls_app_len() > 0) {
                had_app = 1;
                tls_clear_app_ready();
                size_t take = tls_app_len() < ns_conn.cap ? tls_app_len() : ns_conn.cap;
                take = tls_copy_app_data((uint8_t *)ns_conn.buf, take);
                ns_conn.got = take;
                tls_clear_app_len();
                if (!ns_conn.hdr_done && ns_conn.got >= 4) {
                    size_t scan = ns_conn.got < NS_HDR_MAX ? ns_conn.got : NS_HDR_MAX;
                    for (size_t i = 3; i < scan; i++) {
                        if (ns_conn.buf[i - 3] == '\r' && ns_conn.buf[i - 2] == '\n' &&
                            ns_conn.buf[i - 1] == '\r' && ns_conn.buf[i] == '\n') {
                            ns_conn.hdr_done = 1;
                            ns_conn.hdr_end = i + 1;
                            if (ns_conn.hdr_end >= 12 &&
                                ns_conn.buf[0] == 'H' && ns_conn.buf[1] == 'T' &&
                                ns_conn.buf[2] == 'T' && ns_conn.buf[3] == 'P') {
                                int c1 = ns_conn.buf[9];
                                int c2 = ns_conn.buf[10];
                                int c3 = ns_conn.buf[11];
                                if (c1 >= '0' && c1 <= '9' && c2 >= '0' && c2 <= '9' &&
                                    c3 >= '0' && c3 <= '9') {
                                    ns_conn.http_status = (c1 - '0') * 100 +
                                                          (c2 - '0') * 10 + (c3 - '0');
                                }
                            }
                            break;
                        }
                    }
                }
            }
            ns_emit(TCP_ACK | TCP_FIN, ns_conn.snd_nxt, ns_conn.rcv_nxt, NULL, 0);
            klog("[net] https %s status=%d bytes=%lu hdr_done=%d had_app=%d\n",
                 ns_conn.host, ns_conn.http_status, (unsigned long)ns_conn.got,
                 ns_conn.hdr_done, had_app);
            {
                const char *d = ns_conn.buf;
                size_t nn = ns_conn.got < 40 ? ns_conn.got : 40;
                klog("[net] dump: ");
                for (size_t i = 0; i < nn; i++) {
                    klog("%x ", (unsigned)(unsigned char)d[i]);
                }
                klog("\n");
            }
            if (had_app && ns_conn.hdr_done &&
                ns_conn.http_status >= 200 && ns_conn.http_status < 400) {
                ns_finish(NS_OK);
            } else if (had_app) {
                ns_finish(NS_ERR_HTTP);
            } else {
                ns_finish(NS_ERR_CONN);
            }
            return;
        }
        ns_emit(TCP_ACK | TCP_FIN, ns_conn.snd_nxt, ns_conn.rcv_nxt, NULL, 0);
        klog("[net] http %s status=%d bytes=%lu hdr_done=%d\n",
             ns_conn.host, ns_conn.http_status, (unsigned long)ns_conn.got,
             ns_conn.hdr_done);
        {
            const char *d = ns_conn.buf;
            size_t nn = ns_conn.got < 40 ? ns_conn.got : 40;
            klog("[net] dump: ");
            for (size_t i = 0; i < nn; i++) {
                klog("%x ", (unsigned)(unsigned char)d[i]);
            }
            klog("\n");
        }
        ns_finish(ns_conn.http_status >= 200 && ns_conn.http_status < 400
                      ? NS_OK : NS_ERR_HTTP);
        return;
    }

    if (adv) ns_emit(TCP_ACK, ns_conn.snd_nxt, ns_conn.rcv_nxt, NULL, 0);
}

/* ---- RX dispatch ---- */

static void rx_ipv4(const uint8_t *p, size_t n) {
    if (n < IPV4_HLEN) return;
    uint8_t ihl = (uint8_t)((p[0] & 0x0F) * 4);
    if (ihl < IPV4_HLEN || ihl > n) return;
    uint8_t proto = p[9];
    uint16_t tlen = b16(p + 2);
    if (tlen < ihl) return;
    size_t plen = (size_t)tlen - ihl;
    if (plen > n - ihl) plen = n - ihl;
    const uint8_t *pay = p + ihl;
    klog("[net] ip proto=%u ihl=%u tlen=%u plen=%lu dst=%u.%u.%u.%u\n",
         (unsigned)proto, (unsigned)ihl, (unsigned)tlen,
         (unsigned long)plen, p[16], p[17], p[18], p[19]);

    if (proto == 0x11 && plen >= UDP_HLEN) {
        uint16_t dport = b16(pay + 2);
        if (dport == ns_conn.dns_sport && ns_conn.phase == NS_PH_DNS) {
            dns_handle(pay + UDP_HLEN, plen - UDP_HLEN);
        }
    } else if (proto == 0x06 && plen >= TCP_HLEN) {
        uint16_t dport = b16(pay + 2);
        if (ns_conn.active && dport == ns_conn.sport) {
            tcp_handle(pay, plen);
        } else if (ns_conn.active && dport == 443) {
            klog("[net] ip tcp from server dport=%u sport=%u\n",
                 (unsigned)dport, (unsigned)b16(pay));
        }
    }
}

static void rx_frame(const uint8_t *f, size_t n) {
    if (n < ETH_HLEN) return;
    uint16_t type = b16(f + 12);
    klog("[net] frame type=%u len=%lu\n", (unsigned)type, (unsigned long)n);
    const uint8_t *p = f + ETH_HLEN;
    size_t plen = n - ETH_HLEN;

    if (type == ETHERTYPE_IPV4) {
        rx_ipv4(p, plen);
    } else if (type == ETHERTYPE_ARP && plen >= 28) {
        if (b16(p + 6) == 2) {                  /* reply */
            arp_update(p + 14, p + 8);
        }
    }
}

/* ---- public API ---- */

int ns_http_get(const char *host, const char *path,
                char *buf, size_t cap, ns_cb_t cb, void *ctx) {
    if (!net_present() || !net_link_up()) {
        if (cb) cb(ctx, NS_ERR_NONET, 0, 0);
        return -1;
    }

    memset(&ns_conn, 0, sizeof ns_conn);
    ns_conn.active = 1;
    ns_conn.phase = NS_PH_DNS;
    ns_conn.buf = buf;
    ns_conn.cap = cap;
    ns_conn.cb = cb;
    ns_conn.ctx = ctx;

    /* Detect https:// and strip the scheme. */
    if (host && memcmp(host, "https://", 8) == 0) {
        ns_conn.use_tls = 1;
        host += 8;
    } else if (host && memcmp(host, "http://", 7) == 0) {
        ns_conn.use_tls = 0;
        host += 7;
    }

    /* Strip any path/query/fragment from host. */
    if (host) {
        size_t i = 0;
        while (host[i] && host[i] != '/' && host[i] != '?' && host[i] != '#') {
            ns_conn.host[i] = host[i];
            i++;
        }
        ns_conn.host[i] = 0;
    }
    if (path) {
        size_t i = 0;
        while (path[i] && i + 1 < sizeof ns_conn.path) {
            ns_conn.path[i] = path[i];
            i++;
        }
        ns_conn.path[i] = 0;
    }
    ns_port_ctr++;
    ns_conn.sport = (uint16_t)(40000 + (ns_port_ctr % 15000));
    ns_conn.dns_sport = (uint16_t)(50000 + (ns_port_ctr % 10000));
    ns_conn.dns_id = (uint16_t)(0x1000 + (ns_port_ctr % 0x7000));
    ns_conn.isn = (uint32_t)(0x12345678u + timer_get_ticks() * 97u +
                             ns_port_ctr * 7919u);
    ns_conn.snd_nxt = ns_conn.isn + 1;
    ns_conn.deadline = timer_get_ticks() + NS_DEADLINE;
    memcpy(ns_my_mac, net_mac(), 6);
    klog("[net] get %s://%s%s\n", ns_conn.use_tls ? "https" : "http",
         ns_conn.host, ns_conn.path);
    if (ns_conn.use_tls) {
        klog("[https] connecting host=%s port=443\n", ns_conn.host);
    }
    return 0;
}

void ns_abort(void *ctx) {
    if (ns_conn.active && ns_conn.ctx == ctx) {
        ns_conn.active = 0;
        memset(&ns_conn, 0, sizeof ns_conn);
    }
}

int ns_is_active(void) {
    return ns_conn.active;
}

void ns_poll(void) {
    net_poll();
    uint8_t f[1526];
    size_t n;
    while (net_receive(f, sizeof f, &n)) rx_frame(f, n);

    if (!ns_conn.active) return;
    uint64_t now = timer_get_ticks();

    if (now >= ns_conn.deadline) {
        klog("[net] timeout for %s\n", ns_conn.host);
        ns_finish(NS_ERR_CONN);
        return;
    }

    if (ns_conn.phase == NS_PH_DNS) {
        ns_conn.retries = 0;
        if (!ns_conn.dns_sent) {
            if (arp_lookup(ns_dns_ip)) {
                dns_send_query();
                ns_conn.dns_sent = 1;
            } else if ((int)(now - ns_conn.last_send) >= NS_RETRY) {
                arp_request(ns_dns_ip);
                ns_conn.last_send = now;
            }
        } else if ((int)(now - ns_conn.last_send) >= NS_RETRY) {
            if (++ns_conn.retries > 8) {
                klog("[net] dns no reply for %s\n", ns_conn.host);
                ns_finish(NS_ERR_DNS);
                return;
            }
            dns_send_query();
        }
        return;
    }

    if (ns_conn.phase == NS_PH_CONNECT) {
        ns_conn.retries = 0;
        if (!arp_lookup(ns_gw_ip)) {
            if ((int)(now - ns_conn.last_send) >= NS_RETRY) {
                arp_request(ns_gw_ip);
                ns_conn.last_send = now;
            }
            return;
        }
        if (ns_conn.tcp_state != TCP_S_SYN_SENT) {
            ns_conn.tcp_state = TCP_S_SYN_SENT;
            klog("[net] tcp SYN sent to %u.%u.%u.%u\n",
                 ns_conn.dst_ip[0], ns_conn.dst_ip[1],
                 ns_conn.dst_ip[2], ns_conn.dst_ip[3]);
            if (ns_conn.use_tls) {
                klog("[https] SYN sent, awaiting SYN-ACK for TLS\n");
            }
            ns_emit(TCP_SYN, ns_conn.isn, 0, NULL, 0);
            ns_conn.last_send = now;
        } else if ((int)(now - ns_conn.last_send) >= NS_RETRY) {
            if (++ns_conn.retries > 24) {
                klog("[net] tcp connect timeout %s\n", ns_conn.host);
                ns_finish(NS_ERR_CONN);
                return;
            }
            ns_emit(TCP_SYN, ns_conn.isn, 0, NULL, 0);
            ns_conn.last_send = now;
        }
        return;
    }

    if (ns_conn.phase == NS_PH_TLS) {
        if (!ns_conn.tls_sent) {
            tls_poll();
            if (tls_is_established()) {
                klog("[net] TLS established, sending request\n");
                if (ns_conn.use_tls) {
                    klog("[https] sending encrypted HTTP request (%u bytes)\n",
                         (unsigned)ns_conn.req_len);
                }
                ns_send_request();
                ns_conn.tls_sent = 1;
                ns_conn.retries = 0;
            } else if (!tls_is_active()) {
                klog("[net] tls handshake failed %s\n", ns_conn.host);
                ns_finish(NS_ERR_TLS);
                return;
            }
            return;
        }
        if (now >= ns_conn.deadline) {
            klog("[net] tls transfer timeout %s\n", ns_conn.host);
            ns_finish(NS_ERR_CONN);
            return;
        }
        return;
    }

    /* NS_PH_TRANSFER: retransmit the request if nothing has been seen. */
    if (ns_conn.req_sent && !ns_conn.req_acked &&
        (int)(now - ns_conn.last_send) >= NS_RETRY) {
        if (++ns_conn.retries > 10) {
            klog("[net] transfer timeout %s\n", ns_conn.host);
            ns_finish(NS_ERR_CONN);
            return;
        }
        ns_emit(TCP_ACK, ns_conn.snd_nxt - (uint32_t)ns_conn.req_len,
                ns_conn.rcv_nxt, ns_conn.req, ns_conn.req_len);
        ns_conn.last_send = now;
    }
}
