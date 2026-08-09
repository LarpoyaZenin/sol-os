#include "drivers/virtio/virtio_net.h"
#include "drivers/virtio/virtio.h"
#include "drivers/pci/pci.h"
#include "net.h"
#include "klog.h"
#include "mm/pmm.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Virtio-net feature bits (device feature word 0). */
#define VIRTIO_NET_F_CSUM         0u
#define VIRTIO_NET_F_MAC          5u
#define VIRTIO_NET_F_STATUS      16u
#define VIRTIO_NET_F_MRG_RXBUF   15u

/* The virtio-net header is 10 bytes, plus 2 for the num_buffers
 * field when VIRTIO_NET_F_MRG_RXBUF is negotiated. QEMU always
 * writes the 12-byte header on the modern transport, so we negotiate
 * MRG_RXBUF to stay spec-aligned. */
#define VIRTIO_NET_HDR_SIZE      12u     /* struct virtio_net_hdr_mrg_rxbuf */
#define VIRTIO_NET_MTU          1514u
#define VIRTIO_NET_RX_LEN       1526u    /* MTU + header */

#define VIRTIO_NET_S_LINK_UP     0x01u

static virtio_net_t g_net;

/* ---- byte helpers ---- */

static inline uint16_t get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)get_be16(p) << 16) | get_be16(p + 2);
}
static inline void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void put_be32(uint8_t *p, uint32_t v) {
    put_be16(p, (uint16_t)(v >> 16));
    put_be16(p + 2, (uint16_t)v);
}

static uint16_t ip_checksum(const void *data, size_t len) {
    const uint8_t *b = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len >= 2) {
        sum += get_be16(b);
        b += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)(b[0]) << 8;
    while (sum >> 16) sum = (uint16_t)sum + (uint16_t)(sum >> 16);
    return (uint16_t)~sum;
}

/* ---- protocol constants ---- */

#define ETHERTYPE_IPV4   0x0800u
#define ETHERTYPE_ARP    0x0806u
#define ETH_HLEN         14u
#define IPV4_HLEN        20u
#define ICMP_ECHO_REPLY  0u
#define ICMP_ECHO_REQ    8u

/* ---- RX/TX completions ---- */

static void net_drain_raw(void);
static void net_handle_frame(const uint8_t *frame, size_t len);

static void net_irq_handler(void *ctx) {
    (void)ctx;
    uint8_t isr = *(volatile uint8_t *)g_net.vdev.isr;
    if (isr == 0) return;                 /* spurious / other device */
    g_net.irq_count++;
    net_drain_raw();
}

/* Stages one frame copied out of RX slot `id` into the inbox ring. */
static void net_stage_rx(uint16_t id, uint32_t len) {
    uintptr_t buf_vaddr = g_net.hhdm + g_net.rx_phys[id];

    if (len <= VIRTIO_NET_HDR_SIZE || len > VIRTIO_NET_RX_LEN) {
        g_net.rx_dropped++;
        return;
    }
    uint32_t payload = len - VIRTIO_NET_HDR_SIZE;
    if (payload > VIRTIO_NET_MTU) {
        g_net.rx_dropped++;
        return;
    }

    if (g_net.inbox_count == VIRTIO_NET_INBOX) {   /* drop oldest */
        g_net.inbox_head = (uint16_t)((g_net.inbox_head + 1) % VIRTIO_NET_INBOX);
        g_net.inbox_count--;
        g_net.rx_dropped++;
    }

    uint16_t slot = (uint16_t)((g_net.inbox_head + g_net.inbox_count) % VIRTIO_NET_INBOX);
    memcpy((void *)(g_net.hhdm + g_net.inbox_phys[slot]),
           (const void *)(buf_vaddr + VIRTIO_NET_HDR_SIZE), payload);
    g_net.inbox_len[slot] = (uint16_t)payload;
    g_net.inbox_count++;
    g_net.rx_packets++;

    /* Auto-answer ARP + ICMP echo requests for our address. */
    net_handle_frame((const uint8_t *)(g_net.hhdm + g_net.inbox_phys[slot]), payload);
}

/* Drains both virtqueues. Caller is responsible for serialization
 * (interrupts off, or already inside an IRQ handler). */
static void net_drain_raw(void) {
    struct virtio_device *d = &g_net.vdev;
    uint16_t id;
    uint32_t len;

    /* TX completions: release the slot back to the free pool. */
    while (virtio_queue_pop_used(d, VIRTIO_NET_TX_QUEUE, &id, &len)) {
        virtio_queue_free_desc(d, VIRTIO_NET_TX_QUEUE, id);
        g_net.tx_packets++;
    }

    /* RX completions: stage the payload, recycle the slot. */
    int recycled = 0;
    while (virtio_queue_pop_used(d, VIRTIO_NET_RX_QUEUE, &id, &len)) {
        net_stage_rx(id, len);
        virtio_queue_recycle(d, VIRTIO_NET_RX_QUEUE, id);
        recycled = 1;
    }
    if (recycled) virtio_queue_kick(d, VIRTIO_NET_RX_QUEUE);
}

void net_poll(void) {
    if (!g_net.present) return;
    __asm__ volatile ("cli");
    net_drain_raw();
    __asm__ volatile ("sti");
}

/* ---- protocol handling ---- */

static void net_handle_frame(const uint8_t *frame, size_t len) {
    if (len < ETH_HLEN) return;
    const uint8_t *src = frame + 6;
    uint16_t ethertype = get_be16(frame + 12);
    const uint8_t *p = frame + ETH_HLEN;
    size_t plen = len - ETH_HLEN;

    /* IPv4: answer ICMP echo requests for our own address. */
    if (ethertype == ETHERTYPE_IPV4 && plen >= IPV4_HLEN) {
        uint8_t ihl = (uint8_t)(p[0] & 0x0F);
        size_t ihl_bytes = (size_t)ihl * 4;
        if (ihl_bytes < IPV4_HLEN || ihl_bytes > plen) return;
        uint8_t proto = p[9];
        uint32_t dst_ip = get_be32(p + 16);

        if (proto == 0x01 /* ICMP */ && dst_ip == ((uint32_t)g_net.ip[0] << 24 |
                                                    (uint32_t)g_net.ip[1] << 16 |
                                                    (uint32_t)g_net.ip[2] << 8  |
                                                    (uint32_t)g_net.ip[3])) {
            size_t icmp_off = ihl_bytes;
            if (plen < icmp_off + 8) return;
            const uint8_t *icmp = p + icmp_off;
            if (icmp[0] != ICMP_ECHO_REQ) return;

            size_t icmp_len = plen - icmp_off;
            if (icmp_len > 256) icmp_len = 256;

            uint8_t out[ETH_HLEN + IPV4_HLEN + 256];
            memcpy(out + ETH_HLEN, p, ihl_bytes);        /* reuse IPv4 hdr */
            memcpy(out + ETH_HLEN + ihl_bytes, icmp, icmp_len);

            uint8_t *o = out + ETH_HLEN;
            /* The request header was copied verbatim, so its source
             * address (o+12) is the sender and its destination (o+16)
             * is ours; the reply must swap them. The sender's IP comes
             * from the request IP header (p+12), not its MAC. */
            memcpy(o + 12, g_net.ip, 4);                 /* src ip = ours */
            memcpy(o + 16, p + 12, 4);                   /* dst ip = sender */
            o[9] = 0x01;                                 /* ICMP */
            put_be16(o + 10, ip_checksum(o, (uint16_t)ihl_bytes));

            uint8_t *ic = out + ETH_HLEN + ihl_bytes;
            ic[0] = ICMP_ECHO_REPLY;
            ic[1] = 0;
            put_be16(ic + 2, 0);
            put_be16(ic + 2, ip_checksum(ic, (uint16_t)icmp_len));

            memcpy(out, src, 6);                         /* eth dst = sender */
            memcpy(out + 6, g_net.mac, 6);               /* eth src = ours */
            put_be16(out + 12, ETHERTYPE_IPV4);

            net_send(out, ETH_HLEN + ihl_bytes + icmp_len);
        }
    }

    /* ARP: answer who-has for our address. */
    if (ethertype == ETHERTYPE_ARP && plen >= 28) {
        uint16_t op = get_be16(p + 6);
        uint32_t tpa = get_be32(p + 24);
        if (op == 1 /* REQUEST */ && tpa == ((uint32_t)g_net.ip[0] << 24 |
                                             (uint32_t)g_net.ip[1] << 16 |
                                             (uint32_t)g_net.ip[2] << 8  |
                                             (uint32_t)g_net.ip[3])) {
            uint8_t out[ETH_HLEN + 28];
            memcpy(out, src, 6);
            memcpy(out + 6, g_net.mac, 6);
            put_be16(out + 12, ETHERTYPE_ARP);

            uint8_t *a = out + ETH_HLEN;
            put_be16(a + 0, 0x0001);                 /* htype: ethernet */
            put_be16(a + 2, ETHERTYPE_IPV4);         /* ptype */
            a[4] = 6; a[5] = 4;                      /* hlen, plen */
            put_be16(a + 6, 2);                      /* op: REPLY */
            memcpy(a + 8, g_net.mac, 6);             /* sha: ours */
            memcpy(a + 14, g_net.ip, 4);             /* spa: ours */
            memcpy(a + 18, p + 8, 6);                /* tha: requester */
            memcpy(a + 24, p + 14, 4);               /* tpa: requester ip */

            net_send(out, sizeof(out));
        }
    }
}

/* ---- public API ---- */

int net_send(const void *data, size_t len) {
    if (!g_net.present) return -1;
    if (len < 14 || len > VIRTIO_NET_MTU) return -1;
    if ((g_net.status & VIRTIO_NET_S_LINK_UP) == 0) {
        g_net.tx_dropped++;
        return -1;
    }

    struct virtio_device *d = &g_net.vdev;
    uint64_t eflags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(eflags));
    __asm__ volatile ("cli");
    uint16_t id;
    if (virtio_queue_alloc_desc(d, VIRTIO_NET_TX_QUEUE, &id) != 0) {
        __asm__ volatile ("pushq %0; popfq" : : "r"(eflags));
        g_net.tx_dropped++;
        return -1;
    }

    uintptr_t buf = g_net.hhdm + g_net.tx_phys[id];
    memset((void *)buf, 0, VIRTIO_NET_HDR_SIZE);   /* virtio_net_hdr: no offloads */
    memcpy((void *)(buf + VIRTIO_NET_HDR_SIZE), data, len);
    virtio_queue_desc_fill(d, VIRTIO_NET_TX_QUEUE, id, g_net.tx_phys[id],
                           (uint32_t)(VIRTIO_NET_HDR_SIZE + len), 0);
    virtio_queue_submit(d, VIRTIO_NET_TX_QUEUE, id);
    virtio_queue_kick(d, VIRTIO_NET_TX_QUEUE);
    __asm__ volatile ("pushq %0; popfq" : : "r"(eflags));
    return 0;
}

int net_receive(void *buf, size_t cap, size_t *out_len) {
    __asm__ volatile ("cli");
    if (g_net.inbox_count == 0) {
        __asm__ volatile ("sti");
        if (out_len != NULL) *out_len = 0;
        return 0;
    }
    uint16_t slot = g_net.inbox_head;
    size_t n = g_net.inbox_len[slot];
    if (n > cap) n = cap;
    memcpy(buf, (const void *)(g_net.hhdm + g_net.inbox_phys[slot]), n);
    g_net.inbox_head = (uint16_t)((g_net.inbox_head + 1) % VIRTIO_NET_INBOX);
    g_net.inbox_count--;
    __asm__ volatile ("sti");

    if (out_len != NULL) *out_len = n;
    return 1;
}

int net_link_up(void) {
    return (g_net.status & VIRTIO_NET_S_LINK_UP) != 0;
}

const uint8_t *net_mac(void) { return g_net.mac; }

uint64_t net_rx_packet_count(void)  { return g_net.rx_packets; }
uint64_t net_tx_packet_count(void)  { return g_net.tx_packets; }
uint64_t net_rx_dropped_count(void) { return g_net.rx_dropped; }
uint64_t net_tx_dropped_count(void) { return g_net.tx_dropped; }
uint64_t net_irq_count(void)        { return g_net.irq_count; }

int net_init(uint64_t hhdm) {
    if (g_net.present) return 1;
    g_net.hhdm = hhdm;
    g_net.ip[0] = 10; g_net.ip[1] = 0; g_net.ip[2] = 2; g_net.ip[3] = 15;

    const pci_device_t *found = NULL;
    for (uint32_t i = 0; i < pci_device_count(); i++) {
        const pci_device_t *p = pci_device_at(i);
        if (p == NULL) continue;
        /* QEMU exposes the net device under several ids depending on
         * the virtio version (0x1000 transitional legacy, 0x1001
         * legacy, 0x1041 modern). Match on class instead. */
        if (p->vendor == 0x1AF4u && p->base_class == 0x02u && p->sub_class == 0x00u) {
            found = p;
            break;
        }
    }
    if (found == NULL) {
        klog("virtio-net: no NIC found on the bus\n");
        return 0;
    }
    klog("virtio-net: found %x:%x.%u (id %x)\n",
         found->bus, found->dev, found->func, found->device_id);

    if (virtio_device_init(&g_net.vdev, found, hhdm) != 0) {
        klog("virtio-net: transport init failed\n");
        return 0;
    }
    if (g_net.vdev.device_cfg == NULL) {
        klog("virtio-net: no device config region (cannot read MAC)\n");
        return 0;
    }

    virtio_device_set_feature(&g_net.vdev, VIRTIO_NET_F_MAC);
    virtio_device_set_feature(&g_net.vdev, VIRTIO_NET_F_STATUS);
    virtio_device_set_feature(&g_net.vdev, VIRTIO_NET_F_MRG_RXBUF);
    if (virtio_device_finish_features(&g_net.vdev) != 0) {
        klog("virtio-net: feature negotiation failed\n");
        return 0;
    }
    klog("virtio-net: dev features lo=%x hi=%x drv features lo=%x hi=%x\n",
         g_net.vdev.device_features_lo, g_net.vdev.device_features_hi,
         g_net.vdev.driver_features_lo, g_net.vdev.driver_features_hi);

    /* Read the MAC from the device config (offset 0). */
    for (int i = 0; i < 6; i++) {
        g_net.mac[i] = virtio_device_cfg_read8(&g_net.vdev, (uint32_t)i);
    }
    klog("virtio-net: mac %x:%x:%x:%x:%x:%x\n",
         g_net.mac[0], g_net.mac[1], g_net.mac[2],
         g_net.mac[3], g_net.mac[4], g_net.mac[5]);

    /* Set up RX (0) and TX (1). */
    uint16_t rx_size = virtio_queue_size(&g_net.vdev, VIRTIO_NET_RX_QUEUE);
    uint16_t tx_size = virtio_queue_size(&g_net.vdev, VIRTIO_NET_TX_QUEUE);
    if (rx_size > VIRTIO_NET_MAX_QUEUE) rx_size = VIRTIO_NET_MAX_QUEUE;
    if (tx_size > VIRTIO_NET_MAX_QUEUE) tx_size = VIRTIO_NET_MAX_QUEUE;
    klog("virtio-net: device offers rx=%u tx=%u\n", rx_size, tx_size);

    if (virtio_queue_init(&g_net.vdev, VIRTIO_NET_RX_QUEUE, rx_size) != 0 ||
        virtio_queue_init(&g_net.vdev, VIRTIO_NET_TX_QUEUE, tx_size) != 0) {
        klog("virtio-net: virtqueue setup failed\n");
        return 0;
    }

    /* RX slots: one page each, seeded with the net header + room for
     * a full MTU frame. TX slots: one page each for outgoing frames. */
    for (uint16_t n = 0; n < rx_size; n++) {
        uintptr_t page = pmm_alloc_page();
        if (page == 0) {
            klog("virtio-net: out of memory seeding RX\n");
            return 0;
        }
        g_net.rx_phys[n] = page;
        if (virtio_queue_add_buffer(&g_net.vdev, VIRTIO_NET_RX_QUEUE,
                                    page, VIRTIO_NET_RX_LEN, 1) != 0) {
            break;
        }
    }
    virtio_queue_kick(&g_net.vdev, VIRTIO_NET_RX_QUEUE);

    for (uint16_t n = 0; n < tx_size; n++) {
        uintptr_t page = pmm_alloc_page();
        if (page == 0) {
            klog("virtio-net: out of memory seeding TX\n");
            return 0;
        }
        g_net.tx_phys[n] = page;
    }

    for (uint16_t n = 0; n < VIRTIO_NET_INBOX; n++) {
        uintptr_t page = pmm_alloc_page();
        if (page == 0) break;
        g_net.inbox_phys[n] = page;
    }

    /* Poll the link status (device config, offset 6). */
    g_net.status = (uint16_t)(virtio_device_cfg_read8(&g_net.vdev, 6) |
                              (uint16_t)virtio_device_cfg_read8(&g_net.vdev, 7) << 8);
    klog("virtio-net: link %s (status %x)\n",
         (g_net.status & VIRTIO_NET_S_LINK_UP) ? "UP" : "down", g_net.status);

    virtio_device_set_driver_ok(&g_net.vdev);

    /* Hook the IRQ line the firmware programmed for this function. */
    uint8_t irq = pci_config_read8(found->bus, found->dev, found->func, 0x3Cu);
    if (irq != 0 && irq < 16) {
        if (idt_register_irq_handler(irq, net_irq_handler, NULL, &g_net.irq_node)) {
            pic_unmask_irq(irq);
            g_net.use_irq = 1;
            g_net.irq = irq;
        }
    }
    klog("virtio-net: up%s (irq=%u) ip 10.0.2.15\n",
         g_net.use_irq ? " with IRQ" : " (polling)", g_net.irq);

    g_net.present = 1;
    return 1;
}
