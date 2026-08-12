#ifndef SOL_CRYPTO_H
#define SOL_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

/* SHA-256 */
void sha256_init(void);
void sha256_update(const void *data, size_t len);
void sha256_final(uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* AES-128 */
void aes128_set_key(const uint8_t key[16]);
void aes128_encrypt(const uint8_t in[16], uint8_t out[16]);

/* AES-128-GCM */
typedef struct {
    uint32_t h[4];
    uint8_t j0[16];
    uint8_t counter[16];
    uint32_t len_enc[2];
    uint32_t len_aad[2];
    uint8_t buf[16];
    unsigned buf_len;
} gcm_ctx;

void gcm_init(gcm_ctx *ctx, const uint8_t key[16], const uint8_t iv[12]);
void gcm_update(gcm_ctx *ctx, const uint8_t *aad, size_t aad_len);
void gcm_encrypt(gcm_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len);
void gcm_final(uint8_t tag[16], gcm_ctx *ctx);

/* X25519 ECDHE */
void x25519_keygen(uint8_t out[32]);
void x25519_shared(uint8_t out[32], const uint8_t my_priv[32], const uint8_t peer_pub[32]);

#endif
