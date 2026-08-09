#ifndef SOL_VIRTIO_NET_H
#define SOL_VIRTIO_NET_H

#include <stdint.h>
#include "drivers/virtio/virtio.h"
#include "arch/x86_64/idt.h"

#define VIRTIO_NET_RX_QUEUE     0u
#define VIRTIO_NET_TX_QUEUE     1u
#define VIRTIO_NET_MAX_QUEUE   256u

/* Staged receive frames (the driver copies packets out of the RX
 * slots so they can be recycled immediately; the app drains this
 * inbox at its leisure). */
#define VIRTIO_NET_INBOX        32u

/* Virtio-net driver state. One instance (the first NIC on the bus).
 * RX and TX each own one page of buffer space per virtqueue slot;
 * received frames are staged into a small inbox ring so the RX slots
 * can be recycled immediately. */
typedef struct virtio_net {
    uint64_t hhdm;
    int      present;

    struct virtio_device vdev;

    int      use_irq;
    uint8_t  irq;
    irq_node_t irq_node;

    uint8_t  mac[6];
    uint8_t  ip[4];                  /* our IPv4 address (static 10.0.2.15) */
    uint16_t status;                 /* device config link status */

    /* RX slots (indexed by descriptor id). */
    uintptr_t rx_phys[VIRTIO_NET_MAX_QUEUE];

    /* TX slots (indexed by descriptor id). */
    uintptr_t tx_phys[VIRTIO_NET_MAX_QUEUE];

    /* Staged receive inbox (fixed ring, oldest dropped when full). */
    uintptr_t inbox_phys[VIRTIO_NET_INBOX];
    uint16_t  inbox_len[VIRTIO_NET_INBOX];
    uint16_t  inbox_head, inbox_count;

    uint64_t rx_packets, rx_dropped;
    uint64_t tx_packets, tx_dropped;
    uint64_t irq_count;
} virtio_net_t;

#endif /* SOL_VIRTIO_NET_H */
