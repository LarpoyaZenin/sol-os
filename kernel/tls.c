#include "tls.h"
#include "netstack.h"
#include "net.h"
#include "crypto.h"
#include "klog.h"
#include "arch/x86_64/timer.h"
#include <string.h>

/* ---- TLS constants ---- */

#define TLS_VERSION_MAJOR 3
#define TLS_VERSION_MINOR 3

#define TLS_CID_WRITE 1
#define TLS_CID_ALERT 2
#define TLS_CID_HANDSHAKE 22
#define TLS_CID_APPLICATION 23
#define TLS_CID_CHANGE_CIPHER 20

#define TLS_HS_CLIENT_HELLO 1
#define TLS_HS_SERVER_HELLO 2
#define TLS_HS_CERTIFICATE 11
#define TLS_HS_SERVER_KEY_EXCH 12
#define TLS_HS_SERVER_HELLO_DONE 14
#define TLS_HS_CLIENT_KEY_EXCHG 16
#define TLS_HS_CHANGE_CIPHER 20
#define TLS_HS_FINISHED 20

#define TLS_ALERT_WARNING 1
#define TLS_ALERT_FATAL 2
#define TLS_ALERT_CLOSE_NOTIFY 0

#define TLS_CIPHER_ECDHE_RSA_AES128_GCM 0xc02f
#define TLS_CIPHER_ECDHE_RSA_AES256_GCM 0xc02c
#define TLS_CIPHER_RSA_AES_128_GCM 0x002f
#define TLS_CIPHER_RSA_AES_256_GCM 0x0035
#define TLS_EXT_SERVER_NAME 0
#define TLS_EXT_SUPPORTED_GROUPS 10
#define TLS_EXT_SUPPORTED_POINT_FORMATS 11
#define TLS_EXT_SIG_ALGS 13

#define TLS_PRF_LABEL_S "client"
#define TLS_PRF_LABEL_C "server"
#define TLS_PRF_LABEL_FINISHED "tls13 derived"
#define TLS_VERIFY_LABEL "TLS 1.2, Client Finished"

#define TLS_TIMEOUT 2000

static tls_conn g_tls;

/* ---- helpers ---- */

static uint32_t tls_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t tls_be24(const uint8_t *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static void tls_w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint16_t tls_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void tls_w16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static uint64_t tls_now(void) {
    return timer_get_ticks();
}

/* ---- crypto ---- */

static void tls_hmac_sha256(const uint8_t *key, size_t key_len,
                            const uint8_t *msg, size_t msg_len,
                            uint8_t out[32]) {
    uint8_t ipad[64], opad[64];
    memset(ipad, 0x36, 64);
    memset(opad, 0x5C, 64);
    for (size_t i = 0; i < key_len && i < 64; i++) {
        ipad[i] ^= key[i];
        opad[i] ^= key[i];
    }
    uint8_t inner_hash[32];
    sha256_init();
    sha256_update(ipad, 64);
    sha256_update(msg, msg_len);
    sha256_final(inner_hash);
    sha256_init();
    sha256_update(opad, 64);
    sha256_update(inner_hash, 32);
    sha256_final(out);
}

static void tls_prf(const uint8_t *secret, size_t sec_len,
                    const char *label, const uint8_t *seed, size_t seed_len,
                    uint8_t *out, size_t out_len) {
    size_t label_len = strlen(label);
    size_t total_seed = label_len + seed_len;
    uint8_t *hmac_seed = (uint8_t *)__builtin_alloca(total_seed + 64);
    memcpy(hmac_seed, label, label_len);
    memcpy(hmac_seed + label_len, seed, seed_len);
    uint8_t a[32];
    tls_hmac_sha256(secret, sec_len, hmac_seed, total_seed, a);
    size_t off = 0;
    while (off < out_len) {
        uint8_t tmp[64];
        tls_hmac_sha256(secret, sec_len, a, 32, tmp);
        size_t block = out_len - off;
        if (block > 32) block = 32;
        memcpy(out + off, tmp, block);
        tls_hmac_sha256(secret, sec_len, a, 32, a);
        off += block;
    }
}

/* ---- record layer ---- */

static void tls_record_init(uint8_t *p, uint8_t cid, uint16_t len) {
    p[0] = cid;
    p[1] = TLS_VERSION_MAJOR;
    p[2] = TLS_VERSION_MINOR;
    tls_w16(p + 3, len);
}

static int tls_send_raw(const void *data, size_t len) {
    if (!g_tls.active) return -1;
    if (len > 8192) len = 8192;
    ns_tls_send(data, len);
    g_tls.last_send = tls_now();
    return 0;
}

static int tls_send_record(uint8_t cid, const uint8_t *pay, uint16_t plen) {
    uint8_t buf[5 + 1500];
    if (5 + plen > sizeof(buf)) return -1;
    tls_record_init(buf, cid, plen);
    memcpy(buf + 5, pay, plen);
    klog("[tls] send_record: cid=%u ver=%u.%u len=%u total=%u\n", (unsigned)cid,
         (unsigned)buf[1], (unsigned)buf[2], (unsigned)plen, (unsigned)(5 + plen));
    return tls_send_raw(buf, 5 + plen);
}

/* ---- handshake messages ---- */

static void tls_build_client_hello(uint8_t *out, size_t *out_len) {
    uint8_t *p = out;
    size_t host_len;
    
    p[0] = TLS_HS_CLIENT_HELLO;
    p[1] = 0; p[2] = 0; p[3] = 0;
    p += 4;
    p[0] = TLS_VERSION_MAJOR; p[1] = TLS_VERSION_MINOR;
    p += 2;
    memset(p, 0xAB, 32);
    p += 32;
    p[0] = 0;
    p += 1;
    p[0] = 0; p[1] = 0x06;
    p += 2;
    p[0] = (TLS_CIPHER_ECDHE_RSA_AES128_GCM >> 8) & 0xFF;
    p[1] = TLS_CIPHER_ECDHE_RSA_AES128_GCM & 0xFF;
    p += 2;
    p[0] = (TLS_CIPHER_ECDHE_RSA_AES256_GCM >> 8) & 0xFF;
    p[1] = TLS_CIPHER_ECDHE_RSA_AES256_GCM & 0xFF;
    p += 2;
    p[0] = (TLS_CIPHER_RSA_AES_128_GCM >> 8) & 0xFF;
    p[1] = TLS_CIPHER_RSA_AES_128_GCM & 0xFF;
    p += 2;
    p[0] = 0x01;
    p += 1;
    p[0] = 0x00;
    p += 1;
    host_len = 0;
    while (g_tls.host[host_len] && host_len < 96) host_len++;
    klog("[tls] SNI host='%s' host_len=%u bytes=", g_tls.host, (unsigned)host_len);
    for (size_t i = 0; i < host_len && i < 20; i++) klog("%u ", (unsigned)g_tls.host[i]);
    klog("\n");
    size_t sni_data_len = host_len > 0 ? (2 + 1 + 2 + host_len) : 0;
    size_t groups_data_len = 2 + 2 * 2;
    size_t sigalg_data_len = 2 + 3 * 2;
    size_t ecpf_data_len = 1 + 1;
    size_t ext_total = 0;
    if (host_len > 0) ext_total += 4 + sni_data_len;
    ext_total += 4 + groups_data_len;
    ext_total += 4 + sigalg_data_len;
    ext_total += 4 + ecpf_data_len;
    ext_total += 4 + 1; /* extended_master_secret */
    ext_total += 4 + 1; /* renegotiation_info */
    p[0] = (uint8_t)(ext_total >> 8);
    p[1] = (uint8_t)(ext_total);
    p += 2;
    if (host_len > 0) {
        size_t list_len = 3 + host_len;
        p[0] = 0x00; p[1] = 0x00;
        p[2] = (uint8_t)(sni_data_len >> 8);
        p[3] = (uint8_t)(sni_data_len);
        p += 4;
        p[0] = (uint8_t)(list_len >> 8);
        p[1] = (uint8_t)(list_len);
        p += 2;
        p[0] = 0x00;
        p += 1;
        p[0] = (uint8_t)(host_len >> 8);
        p[1] = (uint8_t)(host_len);
        p += 2;
        memcpy(p, g_tls.host, host_len);
        p += host_len;
    }
    p[0] = 0x00; p[1] = 0x0A;
    p[2] = (uint8_t)(groups_data_len >> 8);
    p[3] = (uint8_t)(groups_data_len);
    p += 4;
    p[0] = 0x00; p[1] = 0x17;
    p[2] = 0x00; p[3] = 0x1D;
    p += 4;
    p[0] = 0x00; p[1] = 0x0D;
    p[2] = (uint8_t)(sigalg_data_len >> 8);
    p[3] = (uint8_t)(sigalg_data_len);
    p += 4;
    p[0] = 0x04; p[1] = 0x01;
    p[2] = 0x05; p[3] = 0x01;
    p[4] = 0x04; p[5] = 0x03;
    p += 6;
    p[0] = 0x00; p[1] = 0x0B;
    p[2] = 0x00; p[3] = (uint8_t)ecpf_data_len;
    p += 4;
    p[0] = 0x01;
    p[1] = 0x00;
    p += 2;
    p[0] = 0x00; p[1] = 0x17;
    p[2] = 0x00; p[3] = 0x01;
    p += 4;
    p[0] = 0x00;
    p += 1;
    p[0] = 0xFF; p[1] = 0x01;
    p[2] = 0x00; p[3] = 0x01;
    p += 4;
    p[0] = 0x00;
    p += 1;
    *out_len = (size_t)(p - out);
    out[1] = (uint8_t)((*out_len - 4) >> 16);
    out[2] = (uint8_t)((*out_len - 4) >> 8);
    out[3] = (uint8_t)(*out_len - 4);
}

static void tls_build_client_key_exchange(uint8_t *out, size_t *out_len) {
    uint8_t *p = out;
    p[0] = TLS_HS_CLIENT_KEY_EXCHG;
    p[1] = 0;
    p[2] = 0; p[3] = 0;
    p += 4;
    tls_w16(p, 32);
    p += 2;
    memcpy(p, g_tls.my_pub, 32);
    p += 32;
    *out_len = (size_t)(p - out);
}

static void tls_build_change_cipher(uint8_t *out) {
    out[0] = TLS_HS_CHANGE_CIPHER;
    out[1] = 0; out[2] = 0; out[3] = 0;
}

static void tls_build_finished(uint8_t *out, const char *label) {
    uint8_t seed[64];
    memcpy(seed, label, 13);
    memset(seed + 13, 0, 51);
    uint8_t verify[32];
    tls_prf(g_tls.shared, 32, label, seed, 64, verify, 32);
    out[0] = TLS_HS_FINISHED;
    out[1] = 0; out[2] = 0; out[3] = 12;
    memcpy(out + 4, verify, 12);
}

/* ---- handshake parse ---- */

static int tls_parse_server_hello(const uint8_t *p, size_t n) {
    klog("[tls] parse_sh: p[0]=%u n=%u\n", (unsigned)p[0], (unsigned)n);
    if (n < 38) { klog("[tls] parse_sh: fail n<38\n"); return -1; }
    if (p[0] != TLS_HS_SERVER_HELLO) { klog("[tls] parse_sh: fail type=%u\n", (unsigned)p[0]); return -1; }
    size_t body_len = tls_be24(p + 1);
    klog("[tls] parse_sh: body_len=%u\n", (unsigned)body_len);
    const uint8_t *msg = p + 4;
    if (body_len + 4 > n) { klog("[tls] parse_sh: fail body_len+4>n\n"); return -1; }
    klog("[tls] parse_sh: version=%u.%u\n", (unsigned)msg[0], (unsigned)msg[1]);
    if (msg[0] != TLS_VERSION_MAJOR || msg[1] != TLS_VERSION_MINOR) { klog("[tls] parse_sh: fail version\n"); return -1; }
    return 0;
}

static int tls_parse_server_kex(const uint8_t *p, size_t n) {
    if (n < 4) return -1;
    if (p[0] != TLS_HS_SERVER_KEY_EXCH) return -1;
    const uint8_t *msg = p + 4;
    if (n < 6) return -1;
    if (msg[0] == 3) {
        uint16_t curve = tls_be16(msg + 1);
        if (curve != 0x0017 && curve != 0x001D) return -1;
        if (n < 4) return -1;
        uint8_t point_len = msg[3];
        if (curve == 0x0017) {
            if (point_len != 65 || point_len + 4 > n) return -1;
            memcpy(g_tls.server_pub, msg + 5, 32);
        } else {
            if (point_len != 32 || point_len + 4 > n) return -1;
            memcpy(g_tls.server_pub, msg + 4, 32);
        }
    } else {
        if (n < 5 || msg[3] != 32 || 5 + 32 > n) return -1;
        memcpy(g_tls.server_pub, msg + 4, 32);
    }
    g_tls.server_pub_set = 1;
    return 0;
}

/* ---- key derivation ---- */

static void tls_derive_keys(void) {
    uint8_t seed[64];
    memcpy(seed, "key expansion", 13);
    memset(seed + 13, 0, 51);
    uint8_t key_block[128];
    tls_prf(g_tls.shared, 32, "key expansion", seed, 64, key_block, 128);
    memcpy(g_tls.client_write_key, key_block, 16);
    memcpy(g_tls.server_write_key, key_block + 16, 16);
    memcpy(g_tls.client_write_iv, key_block + 32, 12);
    memcpy(g_tls.server_write_iv, key_block + 44, 12);
    g_tls.client_seq = 0;
    g_tls.server_seq = 0;
}

/* ---- record encryption/decryption ---- */

static void tls_encrypt_record(uint8_t *out, const uint8_t *in, size_t in_len,
                               uint8_t cid, size_t *out_len) {
    gcm_ctx ctx;
    uint8_t iv[12];
    memcpy(iv, g_tls.client_write_iv, 12);
    for (int i = 11; i >= 8; i--) {
        iv[i] = (uint8_t)(g_tls.client_seq >> ((11 - i) * 8));
    }
    gcm_init(&ctx, g_tls.client_write_key, iv);
    uint8_t seq_bytes[8];
    for (int i = 7; i >= 0; i--) seq_bytes[i] = (uint8_t)(g_tls.client_seq >> ((7 - i) * 8));
    gcm_update(&ctx, seq_bytes, 8);
    uint8_t hdr[5];
    tls_record_init(hdr, cid, in_len);
    gcm_update(&ctx, hdr, 5);
    gcm_encrypt(&ctx, in, out + 5, in_len);
    uint8_t tag[16];
    gcm_final(tag, &ctx);
    memcpy(out + 5 + in_len, tag, 16);
    tls_record_init(out, cid, in_len + 16);
    *out_len = 5 + in_len + 16;
    g_tls.client_seq++;
}

static int tls_decrypt_record(const uint8_t *in, size_t in_len,
                              uint8_t *out, size_t *out_len, uint8_t *cid) {
    if (in_len < 5) return -1;
    uint16_t plen = tls_be16(in + 3);
    if (in_len < 5 + plen) return -1;
    if (in[0] != TLS_CID_HANDSHAKE && in[0] != TLS_CID_APPLICATION) return -1;
    *cid = in[0];
    const uint8_t *ct = in + 5;
    uint16_t ct_len = plen - 16;
    if (plen < 16 || ct_len > in_len - 5) return -1;
    gcm_ctx ctx;
    uint8_t iv[12];
    memcpy(iv, g_tls.server_write_iv, 12);
    for (int i = 11; i >= 8; i--) {
        iv[i] = (uint8_t)(g_tls.server_seq >> ((11 - i) * 8));
    }
    gcm_init(&ctx, g_tls.server_write_key, iv);
    uint8_t seq_bytes[8];
    for (int i = 7; i >= 0; i--) seq_bytes[i] = (uint8_t)(g_tls.server_seq >> ((7 - i) * 8));
    gcm_update(&ctx, seq_bytes, 8);
    uint8_t hdr[5];
    tls_record_init(hdr, *cid, ct_len);
    gcm_update(&ctx, hdr, 5);
    uint8_t pt[4096];
    gcm_encrypt(&ctx, ct, pt, ct_len);
    uint8_t expected_tag[16];
    gcm_final(expected_tag, &ctx);
    if (memcmp(expected_tag, ct + ct_len, 16) != 0) return -1;
    *out_len = ct_len;
    memcpy(out, pt, ct_len);
    g_tls.server_seq++;
    return 0;
}

/* ---- send sub-messages ---- */

static int tls_send_handshake(const uint8_t *msg, size_t len) {
    uint8_t buf[512];
    if (len > sizeof(buf)) return -1;
    memcpy(buf, msg, len);
    klog("[tls] send_handshake: msg_len=%u\n", (unsigned)len);
    return tls_send_record(TLS_CID_HANDSHAKE, buf, (uint16_t)len);
}

void tls_send_app_data(const uint8_t *data, size_t len) {
    if (len > 1400) len = 1400;
    uint8_t buf[5 + 1400 + 16];
    gcm_ctx ctx;
    uint8_t iv[12];
    memcpy(iv, g_tls.client_write_iv, 12);
    for (int i = 11; i >= 8; i--) {
        iv[i] = (uint8_t)(g_tls.client_seq >> ((11 - i) * 8));
    }
    gcm_init(&ctx, g_tls.client_write_key, iv);
    uint8_t seq_bytes[8];
    for (int i = 7; i >= 0; i--) seq_bytes[i] = (uint8_t)(g_tls.client_seq >> ((7 - i) * 8));
    gcm_update(&ctx, seq_bytes, 8);
    uint8_t aad_hdr[5];
    tls_record_init(aad_hdr, TLS_CID_APPLICATION, (uint16_t)len);
    gcm_update(&ctx, aad_hdr, 5);
    gcm_encrypt(&ctx, data, buf + 5, len);
    uint8_t tag[16];
    gcm_final(tag, &ctx);
    memcpy(buf + 5 + len, tag, 16);
    tls_record_init(buf, TLS_CID_APPLICATION, (uint16_t)(len + 16));
    ns_tls_send(buf, 5 + len + 16);
    g_tls.client_seq++;
}

static void tls_finish(void) {
    g_tls.app_done = 1;
    if (g_tls.cb) g_tls.cb(g_tls.cb_ctx, 0, 0, 0);
    g_tls.active = 0;
}

/* ---- connection phases ---- */

static void tls_phase_hello(void) {
    uint8_t buf[128];
    size_t blen;
    tls_build_client_hello(buf, &blen);
    klog("[tls] ClientHello (%u bytes): ", (unsigned)blen);
    for (size_t i = 0; i < blen && i < 64; i++) {
        klog("%u ", (unsigned)buf[i]);
    }
    klog("\n");
    klog("[tls] sending ClientHello via tls_send_handshake\n");
    if (tls_send_handshake(buf, blen) == 0) {
        g_tls.phase = 1;
        g_tls.retries = 0;
    }
}

void tls_feed(const uint8_t *data, size_t len) {
    if (!g_tls.active || len == 0) return;
    if (g_tls.phase == 1 || g_tls.phase == 2) {
        if (g_tls.rx_len + len > sizeof(g_tls.rx_buf)) len = sizeof(g_tls.rx_buf) - g_tls.rx_len;
        memcpy(g_tls.rx_buf + g_tls.rx_len, data, len);
        g_tls.rx_len += len;
    }
}

static void tls_phase_server_hello(void) {
    if (g_tls.rx_consumed > 0 && g_tls.rx_consumed < g_tls.rx_len) {
        memmove(g_tls.rx_buf, g_tls.rx_buf + g_tls.rx_consumed,
                g_tls.rx_len - g_tls.rx_consumed);
        g_tls.rx_len -= g_tls.rx_consumed;
    } else if (g_tls.rx_consumed >= g_tls.rx_len) {
        g_tls.rx_len = 0;
    }
    g_tls.rx_consumed = 0;
    klog("[tls] phase_server_hello: rx_len=%lu\n", (unsigned long)g_tls.rx_len);
    if (g_tls.rx_len > 0) {
        klog("[tls] raw rx: ");
        for (size_t i = 0; i < g_tls.rx_len && i < 32; i++) {
            klog("%u ", (unsigned)g_tls.rx_buf[i]);
        }
        klog("\n");
    }
    uint8_t *f = g_tls.rx_buf;
    size_t n = g_tls.rx_len;
    size_t offset = 0;
    size_t parsed_any = 0;
    while (offset + 5 <= n) {
        uint16_t plen = tls_be16(f + offset + 3);
        klog("[tls] loop: offset=%lu n=%lu rec_type=%u plen=%u\n",
             (unsigned long)offset, (unsigned long)n,
             (unsigned)f[offset], (unsigned)plen);
        if (f[offset] == 0x15) {
            klog("[tls] Alert: level=%u desc=%u\n", f[offset + 6], f[offset + 7]);
            g_tls.active = 0;
            if (g_tls.cb) g_tls.cb(g_tls.cb_ctx, -1, 0, 0);
            return;
        }
        if (f[offset] != TLS_CID_HANDSHAKE) {
            klog("[tls] unexpected rec_type %u, breaking\n", (unsigned)f[offset]);
            break;
        }
        if (offset + 5 + plen > n) {
            klog("[tls] incomplete: need %lu have %lu\n",
                 (unsigned long)(offset + 5 + plen), (unsigned long)n);
            break;
        }
        uint8_t msg_type = f[offset + 5];
        klog("[tls] msg_type=%u plen=%u\n", (unsigned)msg_type, (unsigned)plen);
        if (msg_type == TLS_HS_SERVER_HELLO) {
            klog("[tls] got ServerHello\n");
            if (tls_parse_server_hello(f + offset + 5, plen) != 0) break;
            parsed_any = 1;
        } else if (msg_type == TLS_HS_CERTIFICATE) {
            klog("[tls] got Certificate (%u bytes)\n", (unsigned)plen);
            memcpy(g_tls.pending_handshake, f + offset + 5, plen);
            g_tls.pending_len = plen;
            parsed_any = 1;
        } else if (msg_type == TLS_HS_SERVER_KEY_EXCH) {
            klog("[tls] got ServerKeyExchange\n");
            if (tls_parse_server_kex(f + offset + 5, plen) != 0) break;
            parsed_any = 1;
        } else if (msg_type == TLS_HS_SERVER_HELLO_DONE) {
            klog("[tls] got ServerHelloDone\n");
            x25519_keygen(g_tls.my_priv);
            x25519_shared(g_tls.shared, g_tls.my_priv, g_tls.server_pub);
            tls_derive_keys();
            uint8_t ckex[128];
            size_t ckex_len;
            tls_build_client_key_exchange(ckex, &ckex_len);
            tls_send_handshake(ckex, ckex_len);
            uint8_t ccc[4];
            tls_build_change_cipher(ccc);
            tls_send_record(TLS_CID_CHANGE_CIPHER, ccc, 4);
            uint8_t cfin[20];
            tls_build_finished(cfin, TLS_VERIFY_LABEL);
            tls_send_handshake(cfin, 16);
            g_tls.phase = 2;
            g_tls.retries = 0;
            offset += 5 + plen;
            parsed_any = 1;
            if (offset > 0 && offset < n) {
                memmove(g_tls.rx_buf, g_tls.rx_buf + offset, n - offset);
                g_tls.rx_len = n - offset;
            } else if (offset >= n) {
                g_tls.rx_len = 0;
            }
            g_tls.rx_consumed = 0;
            return;
        }
        offset += 5 + plen;
    }
    g_tls.rx_consumed = offset;
    klog("[tls] end_loop: offset=%lu n=%lu parsed=%u rx_consumed=%lu\n",
         (unsigned long)offset, (unsigned long)n, (unsigned)parsed_any,
         (unsigned long)g_tls.rx_consumed);
    if (offset > 0 && offset < n) {
        memmove(g_tls.rx_buf, g_tls.rx_buf + offset, n - offset);
        g_tls.rx_len = n - offset;
    } else if (offset == 0 && n > 0) {
        g_tls.rx_len = n;
    } else {
        g_tls.rx_len = 0;
    }
    g_tls.rx_consumed = 0;
    if (parsed_any) {
        klog("[tls] preserved %lu bytes after handshake msgs\n", (unsigned long)g_tls.rx_len);
    }
}
static void tls_phase_app(void) {
    klog("[tls] phase_app: rx_len=%lu\n", (unsigned long)g_tls.rx_len);
    uint8_t *f = g_tls.rx_buf;
    size_t n = g_tls.rx_len;
    size_t offset = 0;
    size_t rec_len;
    uint8_t pt[4096];
    while (offset + 5 <= n) {
        rec_len = tls_be16(f + offset + 3);
        if (f[offset] == TLS_CID_CHANGE_CIPHER) {
            klog("[tls] phase_app: ChangeCipherSpec\n");
            offset += 5 + rec_len;
            continue;
        }
        if (f[offset] == TLS_CID_HANDSHAKE) {
            uint8_t msg_type = f[offset + 5];
            if (msg_type == TLS_HS_FINISHED) {
                klog("[tls] phase_app: Finished\n");
                offset += 5 + rec_len;
                continue;
            }
            klog("[tls] phase_app: unexpected handshake type %u\n", (unsigned)msg_type);
            break;
        }
        if (f[offset] != TLS_CID_APPLICATION) {
            klog("[tls] phase_app: unexpected rec_type %u, breaking\n", (unsigned)f[offset]);
            break;
        }
        if (offset + 5 + rec_len > n) {
            klog("[tls] phase_app: incomplete app record\n");
            break;
        }
        size_t pt_len;
        if (tls_decrypt_record(f + offset, 5 + rec_len, pt, &pt_len, NULL) == 0) {
            if (g_tls.app_len + pt_len > g_tls.app_cap) pt_len = g_tls.app_cap - g_tls.app_len;
            memcpy(g_tls.app_buf + g_tls.app_len, pt, pt_len);
            g_tls.app_len += pt_len;
        }
        offset += 5 + rec_len;
    }
    if (offset > 0 && offset < n) {
        memmove(g_tls.rx_buf, g_tls.rx_buf + offset, n - offset);
        g_tls.rx_len = n - offset;
    } else if (offset == 0 && n > 0) {
        g_tls.rx_len = n;
    } else {
        g_tls.rx_len = 0;
    }
    if (g_tls.active && g_tls.phase == 2 && !g_tls.app_ready && g_tls.app_len > 0) {
        g_tls.app_ready = 1;
        g_tls.cb_delivered = 0;
    }
    if (g_tls.active && g_tls.phase == 2 && !g_tls.cb_delivered) {
        g_tls.cb_delivered = 1;
        if (g_tls.cb) g_tls.cb(g_tls.cb_ctx, NS_OK, 0, g_tls.app_len);
    }
}

/* ---- public API ---- */

int tls_connect(const char *host, uint8_t *out_buf, size_t cap,
                void (*cb)(void *ctx, int status, size_t off, size_t len),
                void *ctx) {
    (void)out_buf; (void)cap;
    if (!ns_is_active()) {
        if (cb) cb(ctx, -1, 0, 0);
        return -1;
    }
    memset(&g_tls, 0, sizeof g_tls);
    g_tls.active = 1;
    g_tls.phase = 0;
    g_tls.app_cap = cap;
    g_tls.cb = cb;
    g_tls.cb_ctx = ctx;
    size_t i = 0;
    while (host[i] && i + 1 < sizeof g_tls.host) {
        g_tls.host[i] = host[i];
        i++;
    }
    g_tls.host[i] = 0;
    g_tls.deadline = tls_now() + TLS_TIMEOUT;
    tls_phase_hello();
    return 0;
}

void tls_abort(void *ctx) {
    if (g_tls.active && g_tls.cb_ctx == ctx) {
        g_tls.active = 0;
        memset(&g_tls, 0, sizeof g_tls);
    }
}

void tls_poll(void) {
    if (!g_tls.active) return;
    klog("[tls] poll: phase=%d rx_len=%lu\n", g_tls.phase, (unsigned long)g_tls.rx_len);
    uint64_t now = tls_now();
    if (now >= g_tls.deadline) {
        g_tls.active = 0;
        if (g_tls.cb) g_tls.cb(g_tls.cb_ctx, -1, 0, 0);
        return;
    }
    if (g_tls.phase == 0) tls_phase_hello();
    else if (g_tls.phase == 1) {
        tls_phase_server_hello();
        if (g_tls.phase == 2 && g_tls.rx_len > 0) {
            tls_phase_app();
        }
    } else if (g_tls.phase == 2) tls_phase_app();
}

int tls_is_established(void) {
    return g_tls.active && g_tls.phase == 2;
}

int tls_is_active(void) {
    return g_tls.active;
}

int tls_app_ready(void) {
    return g_tls.app_ready;
}

void tls_clear_app_ready(void) {
    g_tls.app_ready = 0;
}

size_t tls_app_len(void) {
    return g_tls.app_len;
}

void tls_clear_app_len(void) {
    g_tls.app_len = 0;
}

size_t tls_copy_app_data(uint8_t *buf, size_t cap) {
    size_t copy = g_tls.app_len < cap ? g_tls.app_len : cap;
    if (copy > 0) {
        memcpy(buf, g_tls.app_buf, copy);
    }
    return copy;
}
