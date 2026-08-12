#ifndef SOL_NETSTACK_H
#define SOL_NETSTACK_H

#include <stddef.h>
#include <stdint.h>

/* Minimal internet client stack sitting on top of the VirtIO-net NIC
 * (net.h): ARP cache + client, IPv4, UDP (for DNS), a single TCP
 * client connection, DNS A-record lookup, and an HTTP/1.0 GET. All of
 * it is poll-driven from ns_poll(); nothing blocks. Only one request
 * may be in flight at a time; starting a new one aborts the old. */

#define NS_OK          0   /* body delivered */
#define NS_ERR_NONET   1   /* no NIC / link down */
#define NS_ERR_DNS     2   /* could not resolve the host name */
#define NS_ERR_CONN    3   /* TCP connect failed (RST or timeout) */
#define NS_ERR_HTTP    4   /* HTTP error status, or header too large */
#define NS_ERR_ABORT   5   /* replaced by a newer request */
#define NS_ERR_TLS     6   /* TLS handshake failed */

typedef void (*ns_cb_t)(void *ctx, int status, size_t off, size_t len);

/* Connection phases. */
#define NS_PH_DNS      0
#define NS_PH_CONNECT  1
#define NS_PH_TLS      2
#define NS_PH_TRANSFER 3

/* Fetches http://host/path or https://host/path over TCP. For HTTPS,
 * the TCP connection is upgraded with a TLS 1.2 handshake before the
 * HTTP request is sent. Response bytes are written into `buf` (at most
 * `cap`); the body begins at offset `off` of `buf` and is `len` bytes
 * long. The callback is invoked exactly once when the request
 * completes. Returns 0 if accepted, -1 if rejected before dispatch. */
int ns_http_get(const char *host, const char *path,
                char *buf, size_t cap, ns_cb_t cb, void *ctx);

/* Aborts any in-flight request belonging to `ctx` without invoking
 * the callback. Safe to call any time. */
void ns_abort(void *ctx);

/* Drives the whole stack: drains RX, dispatches ARP/UDP/TCP, runs DNS,
 * TLS handshake, and TCP retransmit timers. Call once per main-loop
 * iteration. */
void ns_poll(void);

/* Sends raw TCP payload over an established TLS connection. Called by
 * the TLS layer to transmit handshake and application records. */
void ns_tls_send(const void *data, size_t len);

/* Returns 1 if a request is currently active. */
int ns_is_active(void);

#endif /* SOL_NETSTACK_H */
