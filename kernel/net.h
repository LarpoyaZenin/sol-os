#ifndef SOL_NET_H
#define SOL_NET_H

#include <stddef.h>
#include <stdint.h>

/* Network stack (currently: one VirtIO-net NIC + a minimal
 * Ethernet/ARP/IPv4/ICMP responder). The driver is poll/IRQ hybrid:
 * the main loop drains both virtqueues through net_poll(), and an
 * optional IRQ handler does the same when the device has one. */

/* Probes the PCI bus for a VirtIO-net NIC and brings it up: feature
 * negotiation (MAC + STATUS + VERSION_1), RX/TX virtqueues, RX
 * buffers, DRIVER_OK, and the IRQ hook. Must be called after pci_init
 * and with the HHDM offset from the bootloader. Returns 1 if a NIC is
 * running, 0 otherwise. */
int net_init(uint64_t hhdm);

/* Queues a full Ethernet frame (dst+src MAC, ethertype, payload) for
 * transmission. The frame is copied into a driver-owned buffer, so
 * the caller may reuse `data` immediately. Returns 0 on success, -1
 * if the frame is malformed/too long or the TX queue is full. */
int net_send(const void *data, size_t len);

/* Drains RX and TX completions. Safe to call from the main loop;
 * interrupts are disabled internally. Call this regularly so inbound
 * packets are copied out and TX slots are reclaimed. */
void net_poll(void);

/* Copies the next received frame (Ethernet payload only) into `buf`
 * (at most `cap` bytes) and stores its length in *out_len. Returns 1
 * if a frame was copied, 0 if the receive queue is empty. */
int net_receive(void *buf, size_t cap, size_t *out_len);

/* 1 if the NIC reports the link up, 0 otherwise. */
int net_link_up(void);

/* 1 if a NIC was found and brought up by net_init. */
int net_present(void);

/* The NIC's MAC address (6 bytes, valid once net_init succeeded). */
const uint8_t *net_mac(void);

/* This host's IPv4 address (4 bytes, currently 10.0.2.15). */
const uint8_t *net_ip(void);

/* Diagnostics counters. */
uint64_t net_rx_packet_count(void);
uint64_t net_tx_packet_count(void);
uint64_t net_rx_dropped_count(void);
uint64_t net_tx_dropped_count(void);
uint64_t net_irq_count(void);

#endif /* SOL_NET_H */
