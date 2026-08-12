#ifndef SOL_TLS_H
#define SOL_TLS_H

#include <stdint.h>
#include <stddef.h>

/* TLS 1.2 client state. */
typedef struct {
    int active;
    int phase;
    uint64_t deadline;
    int retries;

    /* server */
    char host[96];
    uint8_t server_pub[32];
    int server_pub_set;

    /* key material */
    uint8_t my_priv[32];
    uint8_t my_pub[32];
    uint8_t shared[32];
    uint8_t client_write_key[16];
    uint8_t server_write_key[16];
    uint8_t client_write_iv[12];
    uint8_t server_write_iv[12];
    uint64_t client_seq;
    uint64_t server_seq;

    /* buffers */
    uint8_t rx_buf[8192];
    size_t rx_len;
    size_t rx_consumed;
    uint8_t app_buf[8192];
    size_t app_len;
    size_t app_cap;
    int app_done;
    int app_err;

    /* callbacks */
    void (*cb)(void *ctx, int status, size_t off, size_t len);
    void *cb_ctx;

    /* internal */
    uint8_t pending_handshake[4096];
    size_t pending_len;
    uint64_t last_send;
    int app_ready;
    int cb_delivered;
} tls_conn;

int tls_connect(const char *host, uint8_t *out_buf, size_t cap,
                void (*cb)(void *ctx, int status, size_t off, size_t len),
                void *ctx);
void tls_abort(void *ctx);
void tls_poll(void);
int tls_is_established(void);
int tls_is_active(void);
void tls_feed(const uint8_t *data, size_t len);
int tls_app_ready(void);
void tls_clear_app_ready(void);
size_t tls_app_len(void);
void tls_clear_app_len(void);
size_t tls_copy_app_data(uint8_t *buf, size_t cap);
void tls_send_app_data(const uint8_t *data, size_t len);

#endif
