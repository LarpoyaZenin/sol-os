#include "crypto.h"
#include <string.h>

/* ---- SHA-256 ---- */

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

struct sha256_ctx {
    uint32_t state[8];
    uint64_t bits;
    uint8_t buf[64];
    unsigned buf_len;
};

static struct sha256_ctx g_sha256_ctx;

static void sha256_transform(struct sha256_ctx *s, const uint8_t block[64]) {
    uint32_t w[64];
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4] << 24) |
               ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) |
               ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = (w[i-15] >> 7) | (w[i-15] << 25);
        uint32_t s1 = (w[i-2] >> 17) | (w[i-2] << 15);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=s->state[0], b=s->state[1], c=s->state[2], d=s->state[3];
    uint32_t e=s->state[4], f=s->state[5], g=s->state[6], h=s->state[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = (e >> 6) | (e << 26);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = (a >> 2) | (a << 30);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    s->state[0] += a; s->state[1] += b; s->state[2] += c; s->state[3] += d;
    s->state[4] += e; s->state[5] += f; s->state[6] += g; s->state[7] += h;
}

void sha256_init(void) {
    struct sha256_ctx *s = &g_sha256_ctx;
    s->state[0] = 0x6a09e667; s->state[1] = 0xbb67ae85;
    s->state[2] = 0x3c6ef372; s->state[3] = 0xa54ff53a;
    s->state[4] = 0x510e527f; s->state[5] = 0x9b05688c;
    s->state[6] = 0x1f83d9ab; s->state[7] = 0x5be0cd19;
    s->bits = 0; s->buf_len = 0;
}

void sha256_update(const void *data, size_t len) {
    struct sha256_ctx *s = &g_sha256_ctx;
    s->bits += (uint64_t)len * 8;
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        unsigned need = 64 - s->buf_len;
        unsigned take = need < len ? need : (unsigned)len;
        memcpy(s->buf + s->buf_len, p, take);
        s->buf_len += take;
        p += take; len -= take;
        if (s->buf_len == 64) {
            sha256_transform(s, s->buf);
            s->buf_len = 0;
        }
    }
}

void sha256_final(uint8_t out[32]) {
    struct sha256_ctx *s = &g_sha256_ctx;
    uint64_t bits = s->bits;
    s->buf[s->buf_len++] = 0x80;
    if (s->buf_len > 56) {
        while (s->buf_len < 64) s->buf[s->buf_len++] = 0;
        sha256_transform(s, s->buf);
        s->buf_len = 0;
    }
    while (s->buf_len < 56) s->buf[s->buf_len++] = 0;
    for (int i = 7; i >= 0; i--) {
        s->buf[s->buf_len++] = (uint8_t)(bits >> (i * 8));
    }
    sha256_transform(s, s->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s->state[i] >> 24);
        out[i*4+1] = (uint8_t)(s->state[i] >> 16);
        out[i*4+2] = (uint8_t)(s->state[i] >> 8);
        out[i*4+3] = (uint8_t)(s->state[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
    sha256_init();
    sha256_update(data, len);
    sha256_final(out);
}

/* ---- AES-128 ---- */

static uint32_t aes_rk[44];
static int aes_key_set;

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint32_t aes_xtime(uint32_t v) {
    return (v << 1) ^ ((v >> 7) * 0x1b);
}

static void aes_key_expand(const uint8_t key[16]) {
    uint32_t rk[44];
    int i;
    rk[0] = ((uint32_t)key[0]<<24)|((uint32_t)key[1]<<16)|((uint32_t)key[2]<<8)|key[3];
    rk[1] = ((uint32_t)key[4]<<24)|((uint32_t)key[5]<<16)|((uint32_t)key[6]<<8)|key[7];
    rk[2] = ((uint32_t)key[8]<<24)|((uint32_t)key[9]<<16)|((uint32_t)key[10]<<8)|key[11];
    rk[3] = ((uint32_t)key[12]<<24)|((uint32_t)key[13]<<16)|((uint32_t)key[14]<<8)|key[15];
    for (i = 4; i < 44; i++) {
        uint32_t t = rk[i-1];
        if (i % 4 == 0) {
            t = ((t << 8) | (t >> 24));
            uint8_t b0 = (uint8_t)(t >> 24);
            uint8_t b1 = (uint8_t)(t >> 16);
            uint8_t b2 = (uint8_t)(t >> 8);
            uint8_t b3 = (uint8_t)(t);
            t = ((uint32_t)aes_sbox[b0]<<24)|((uint32_t)aes_sbox[b1]<<16)|
                ((uint32_t)aes_sbox[b2]<<8)|aes_sbox[b3];
            t ^= (uint32_t)(0x1u << ((i/4 - 1) % 8));
        }
        rk[i] = rk[i-4] ^ t;
    }
    memcpy(aes_rk, rk, sizeof(aes_rk));
    aes_key_set = 1;
}

void aes128_set_key(const uint8_t key[16]) {
    aes_key_expand(key);
}

static void aes_add_round_key(uint32_t s[4], int round) {
    s[0] ^= aes_rk[round*4];
    s[1] ^= aes_rk[round*4+1];
    s[2] ^= aes_rk[round*4+2];
    s[3] ^= aes_rk[round*4+3];
}

static void aes_sub_bytes(uint32_t s[4]) {
    int i;
    for (i = 0; i < 4; i++) {
        uint8_t b0 = (uint8_t)(s[i] >> 24);
        uint8_t b1 = (uint8_t)(s[i] >> 16);
        uint8_t b2 = (uint8_t)(s[i] >> 8);
        uint8_t b3 = (uint8_t)(s[i]);
        s[i] = ((uint32_t)aes_sbox[b0]<<24)|((uint32_t)aes_sbox[b1]<<16)|
               ((uint32_t)aes_sbox[b2]<<8)|aes_sbox[b3];
    }
}

static void aes_shift_rows(uint32_t s[4]) {
    uint32_t t = s[1];
    s[1] = (t >> 8) | (t << 24);
    t = s[2];
    s[2] = (t >> 16) | (t << 16);
    t = s[3];
    s[3] = (t >> 24) | (t << 8);
}

static void aes_mix_columns(uint32_t s[4]) {
    int i;
    for (i = 0; i < 4; i++) {
        uint32_t v = s[i];
        uint32_t a = (uint8_t)(v >> 24);
        uint32_t b = (uint8_t)(v >> 16);
        uint32_t c = (uint8_t)(v >> 8);
        uint32_t d = (uint8_t)(v);
        s[i] = (aes_xtime(a) ^ aes_xtime(b) ^ b ^ c ^ d) << 24 |
               (a ^ aes_xtime(b) ^ aes_xtime(c) ^ c ^ d) << 16 |
               (a ^ b ^ aes_xtime(c) ^ aes_xtime(d) ^ d) << 8 |
               (aes_xtime(a) ^ a ^ b ^ c ^ aes_xtime(d));
    }
}

void aes128_encrypt(const uint8_t in[16], uint8_t out[16]) {
    if (!aes_key_set) return;
    uint32_t s[4];
    int i;
    s[0] = ((uint32_t)in[0]<<24)|((uint32_t)in[1]<<16)|((uint32_t)in[2]<<8)|in[3];
    s[1] = ((uint32_t)in[4]<<24)|((uint32_t)in[5]<<16)|((uint32_t)in[6]<<8)|in[7];
    s[2] = ((uint32_t)in[8]<<24)|((uint32_t)in[9]<<16)|((uint32_t)in[10]<<8)|in[11];
    s[3] = ((uint32_t)in[12]<<24)|((uint32_t)in[13]<<16)|((uint32_t)in[14]<<8)|in[15];
    aes_add_round_key(s, 0);
    for (i = 1; i < 10; i++) {
        aes_sub_bytes(s);
        aes_shift_rows(s);
        aes_mix_columns(s);
        aes_add_round_key(s, i);
    }
    aes_sub_bytes(s);
    aes_shift_rows(s);
    aes_add_round_key(s, 10);
    for (i = 0; i < 4; i++) {
        out[i*4]   = (uint8_t)(s[i] >> 24);
        out[i*4+1] = (uint8_t)(s[i] >> 16);
        out[i*4+2] = (uint8_t)(s[i] >> 8);
        out[i*4+3] = (uint8_t)(s[i]);
    }
}

/* ---- GHASH ---- */

static void ghash_mul(uint32_t r[4], const uint32_t x[4]) {
    uint32_t z[4] = {0,0,0,0};
    uint32_t v[4];
    int i, j;
    memcpy(v, r, 16);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 32; j++) {
            if ((x[i] >> (31 - j)) & 1) {
                z[0] ^= v[0]; z[1] ^= v[1]; z[2] ^= v[2]; z[3] ^= v[3];
            }
            uint32_t carry = v[3] & 1;
            v[3] = (v[3] >> 1) | (v[2] << 31);
            v[2] = (v[2] >> 1) | (v[1] << 31);
            v[1] = (v[1] >> 1) | (v[0] << 31);
            v[0] = (v[0] >> 1) ^ (carry ? 0xe1000000u : 0);
        }
    }
    memcpy(r, z, 16);
}

/* ---- AES-128-GCM ---- */

void gcm_init(gcm_ctx *ctx, const uint8_t key[16], const uint8_t iv[12]) {
    uint8_t zero[16] = {0};
    uint8_t h_block[16];
    aes128_set_key(key);
    aes128_encrypt(zero, h_block);
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->h, h_block, 16);
    memcpy(ctx->j0, iv, 12);
    ctx->j0[12] = 0; ctx->j0[13] = 0; ctx->j0[14] = 0; ctx->j0[15] = 1;
}

void gcm_update(gcm_ctx *ctx, const uint8_t *aad, size_t aad_len) {
    ctx->len_aad[0] = (uint32_t)(aad_len * 8);
    ctx->len_aad[1] = (uint32_t)((uint64_t)aad_len >> 29);
    while (aad_len > 0) {
        unsigned need = 16 - ctx->buf_len;
        unsigned take = need < aad_len ? need : (unsigned)aad_len;
        memcpy(ctx->buf + ctx->buf_len, aad, take);
        ctx->buf_len += take;
        aad += take; aad_len -= take;
        if (ctx->buf_len == 16) {
            int i;
            for (i = 0; i < 4; i++)
                ctx->h[i] ^= ((uint32_t)ctx->buf[i*4]<<24)|((uint32_t)ctx->buf[i*4+1]<<16)|
                              ((uint32_t)ctx->buf[i*4+2]<<8)|ctx->buf[i*4+3];
            ghash_mul(ctx->h, ctx->h);
            ctx->buf_len = 0;
        }
    }
}

static void gcm_inc_counter(uint8_t ctr[16]) {
    int i;
    for (i = 15; i >= 12; i--) {
        ctr[i]++;
        if (ctr[i] != 0) break;
    }
}

void gcm_encrypt(gcm_ctx *ctx, const uint8_t *in, uint8_t *out, size_t len) {
    ctx->len_enc[0] = (uint32_t)(len * 8);
    ctx->len_enc[1] = (uint32_t)((uint64_t)len >> 29);
    while (len > 0) {
        unsigned need = 16 - ctx->buf_len;
        unsigned take = need < len ? need : (unsigned)len;
        memcpy(ctx->buf + ctx->buf_len, in, take);
        ctx->buf_len += take;
        in += take; len -= take;
        if (ctx->buf_len == 16) {
            int i;
            gcm_inc_counter(ctx->counter);
            uint8_t keystream[16];
            aes128_encrypt(ctx->counter, keystream);
            for (i = 0; i < 16; i++) ctx->buf[i] ^= keystream[i];
            for (i = 0; i < 4; i++)
                ctx->h[i] ^= ((uint32_t)ctx->buf[i*4]<<24)|((uint32_t)ctx->buf[i*4+1]<<16)|
                              ((uint32_t)ctx->buf[i*4+2]<<8)|ctx->buf[i*4+3];
            ghash_mul(ctx->h, ctx->h);
            memcpy(out - ctx->buf_len + 16, ctx->buf, 16);
            ctx->buf_len = 0;
        }
    }
}

void gcm_final(uint8_t tag[16], gcm_ctx *ctx) {
    int i;
    if (ctx->buf_len > 0) {
        gcm_inc_counter(ctx->counter);
        uint8_t keystream[16] = {0};
        aes128_encrypt(ctx->counter, keystream);
        for (i = ctx->buf_len; i < 16; i++) ctx->buf[i] = 0;
        for (i = 0; i < 16; i++) ctx->buf[i] ^= keystream[i];
        for (i = 0; i < 4; i++)
            ctx->h[i] ^= ((uint32_t)ctx->buf[i*4]<<24)|((uint32_t)ctx->buf[i*4+1]<<16)|
                          ((uint32_t)ctx->buf[i*4+2]<<8)|ctx->buf[i*4+3];
        ghash_mul(ctx->h, ctx->h);
    }
    uint8_t len_buf[16];
    memcpy(len_buf, ctx->len_aad, 4);
    memcpy(len_buf+4, ctx->len_enc, 4);
    memset(len_buf+8, 0, 8);
    for (i = 0; i < 4; i++)
        ctx->h[i] ^= ((uint32_t)len_buf[i*4]<<24)|((uint32_t)len_buf[i*4+1]<<16)|
                      ((uint32_t)len_buf[i*4+2]<<8)|len_buf[i*4+3];
    ghash_mul(ctx->h, ctx->h);
    uint8_t j0[16];
    memcpy(j0, ctx->j0, 16);
    for (i = 0; i < 4; i++)
        ctx->h[i] ^= ((uint32_t)j0[i*4]<<24)|((uint32_t)j0[i*4+1]<<16)|
                      ((uint32_t)j0[i*4+2]<<8)|j0[i*4+3];
    for (i = 0; i < 4; i++) {
        tag[i*4]   = (uint8_t)(ctx->h[i] >> 24);
        tag[i*4+1] = (uint8_t)(ctx->h[i] >> 16);
        tag[i*4+2] = (uint8_t)(ctx->h[i] >> 8);
        tag[i*4+3] = (uint8_t)(ctx->h[i]);
    }
}

static uint32_t tls_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|p[3];
}

/* ---- RSA ---- */

static uint32_t mod_mul(uint32_t a, uint32_t b, uint32_t mod) {
    uint64_t r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | ((a >> 31) & 1);
        a = (a << 1) | ((b >> 31) & 1);
        b = b << 1;
        if (r >= mod) r -= mod;
    }
    return (uint32_t)r;
}

static uint32_t mod_pow(uint32_t base, uint32_t exp, uint32_t mod) {
    uint32_t r = 1;
    while (exp > 0) {
        if (exp & 1) r = mod_mul(r, base, mod);
        base = mod_mul(base, base, mod);
        exp >>= 1;
    }
    return r;
}

static uint32_t mod_inv(uint32_t a, uint32_t mod) {
    int32_t t[2] = {0, 1};
    int32_t r[2] = {mod, (int32_t)a};
    while (r[1] != 0) {
        int32_t q = r[0] / r[1];
        int32_t nt = t[0] - q * t[1];
        int32_t nr = r[0] - q * r[1];
        t[0] = t[1]; t[1] = nt;
        r[0] = r[1]; r[1] = nr;
    }
    if (t[0] < 0) t[0] += mod;
    return (uint32_t)t[0];
}

int rsa_verify(const uint8_t *msg, size_t msg_len, const uint8_t *sig, size_t sig_len,
               const uint8_t *e, size_t e_len, const uint8_t *n, size_t n_len) {
    if (sig_len != n_len) return 0;
    uint8_t dec[512];
    if (n_len > sizeof(dec)) return 0;
    memcpy(dec, sig, n_len);
    uint32_t e_val = 0;
    for (size_t i = 0; i < e_len && i < 4; i++) {
        e_val = (e_val << 8) | e[i];
    }
    int n_words = (int)(n_len / 4);
    uint32_t n_val[128];
    for (int i = 0; i < n_words; i++) {
        n_val[i] = tls_be32(n + i * 4);
    }
    for (int i = 0; i < n_words; i++) {
        dec[i * 4] = (uint8_t)(dec[i * 4] ^ 0x00);
    }
    uint32_t result[128] = {0};
    result[0] = 1;
    for (int i = n_words - 1; i >= 0; i--) {
        uint32_t carry = 0;
        for (int j = n_words - 1; j >= 0; j--) {
            uint64_t prod = (uint64_t)result[j] * dec[i * 4] + carry;
            uint32_t new_val = (uint32_t)(prod >> 32);
            uint32_t low = (uint32_t)prod;
            for (int k = j; k < n_words; k++) {
                uint64_t sum = (uint64_t)result[k] + n_val[k] * new_val + ((uint64_t)low << 32);
                low = (uint32_t)(sum >> 32);
                result[k] = (uint32_t)sum;
                if (k == j && low == 0 && result[k] < dec[i * 4]) break;
            }
            carry = new_val;
        }
    }
    uint8_t result_bytes[512];
    memset(result_bytes, 0, n_len);
    for (int i = 0; i < n_words; i++) {
        result_bytes[i * 4] = (uint8_t)(result[i] >> 24);
        result_bytes[i * 4 + 1] = (uint8_t)(result[i] >> 16);
        result_bytes[i * 4 + 2] = (uint8_t)(result[i] >> 8);
        result_bytes[i * 4 + 3] = (uint8_t)(result[i]);
    }
    if (result_bytes[0] != 0x00 || result_bytes[1] != 0x01) return 0;
    size_t ps_len = 2;
    while (ps_len < n_len && result_bytes[ps_len] == 0xFF) ps_len++;
    if (ps_len < 2 || result_bytes[ps_len] != 0x00) return 0;
    size_t di_off = ps_len + 1;
    static const uint8_t sha256_digest_info[] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,
        0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,
        0x00,0x04,0x20
    };
    if (di_off + sizeof(sha256_digest_info) + 32 > n_len) return 0;
    if (memcmp(result_bytes + di_off, sha256_digest_info, sizeof(sha256_digest_info)) != 0)
        return 0;
    uint8_t expected[32];
    sha256(msg, msg_len, expected);
    return memcmp(result_bytes + di_off + sizeof(sha256_digest_info), expected, 32) == 0;
}

/* ---- X25519 (RFC 7748, radix-2^51 limbs) ---- */

#define X25519_LIMB_BITS 51
#define X25519_LIMB_MASK ((1ull << X25519_LIMB_BITS) - 1ull)

typedef struct { uint32_t v[10]; } x25519_fe;

static void x25519_fe_set(x25519_fe *r, uint32_t a0) {
    memset(r, 0, sizeof(*r));
    r->v[0] = (uint32_t)((uint64_t)a0 & X25519_LIMB_MASK);
}

static uint32_t x25519_fe_load(const uint8_t in[32], x25519_fe *r) {
    uint64_t t[10];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < 32; i++) t[i / 5] += (uint64_t)in[i] << (8 * (i % 5));
    r->v[0] = (uint32_t)(t[0] & X25519_LIMB_MASK);
    r->v[1] = (uint32_t)(((t[0] >> 51) + (t[1] << 3)) & X25519_LIMB_MASK);
    r->v[2] = (uint32_t)(((t[1] >> 48) + (t[2] << 6)) & X25519_LIMB_MASK);
    r->v[3] = (uint32_t)(((t[2] >> 45) + (t[3] << 1)) & X25519_LIMB_MASK);
    r->v[4] = (uint32_t)(((t[3] >> 50) + (t[4] << 4)) & X25519_LIMB_MASK);
    r->v[5] = (uint32_t)(((t[4] >> 47) + (t[5] << 7)) & X25519_LIMB_MASK);
    r->v[6] = (uint32_t)(((t[5] >> 44) + (t[6] << 2)) & X25519_LIMB_MASK);
    r->v[7] = (uint32_t)(((t[6] >> 49) + (t[7] << 5)) & X25519_LIMB_MASK);
    r->v[8] = (uint32_t)(((t[7] >> 46) + (t[8] << 0)) & X25519_LIMB_MASK);
    r->v[9] = (uint32_t)(((t[8] >> 51) + (t[9] << 3)) & X25519_LIMB_MASK);
    return (uint32_t)((t[9] >> 48) & 0x7fffffull);
}

static void x25519_fe_store(const x25519_fe *a, uint8_t out[32]) {
    uint64_t t[10];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < 10; i++) t[i] = a->v[i];
    uint64_t mask = X25519_LIMB_MASK;
    for (int i = 0; i < 10; i++) { t[i + 1] += t[i] >> 51; t[i] &= mask; }
    t[9] &= 0x7fffffffull;
    t[0] += 19 * (t[9] >> 51);
    for (int i = 0; i < 10; i++) { t[i + 1] += t[i] >> 51; t[i] &= mask; }
    t[9] &= 0x7fffffffull;
    for (int i = 0; i < 10; i++) {
        uint64_t limb = t[i];
        for (int j = 0; j < 6 && (i * 5 + j) < 32; j++)
            out[i * 5 + j] = (uint8_t)(limb >> (j * 8));
    }
}

static void x25519_fe_mul(x25519_fe *r, const x25519_fe *a, const x25519_fe *b) {
    uint64_t t[20];
    memset(t, 0, sizeof(t));
    for (int i = 0; i < 10; i++)
        for (int j = 0; j < 10; j++)
            t[i + j] += (uint64_t)a->v[i] * (uint64_t)b->v[j];
    uint64_t mask = X25519_LIMB_MASK;
    for (int i = 0; i < 19; i++) { t[i + 1] += t[i] >> 51; t[i] &= mask; }
    t[19] &= 0x7fffffffull;
    t[0] += 19 * (t[19] >> 51);
    for (int i = 0; i < 19; i++) { t[i + 1] += t[i] >> 51; t[i] &= mask; }
    t[19] &= 0x7fffffffull;
    for (int i = 0; i < 10; i++) r->v[i] = (uint32_t)t[i];
}

static void x25519_fe_sq(x25519_fe *r, const x25519_fe *a) { x25519_fe_mul(r, a, a); }

static void x25519_fe_inv(x25519_fe *r, const x25519_fe *a) {
    x25519_fe t[10];
    memcpy(t, a, sizeof(*a));
    for (int i = 0; i < 253; i++) x25519_fe_sq(t, t);
    x25519_fe_sq(r, t);
    x25519_fe_mul(r, r, a);
    for (int i = 0; i < 252; i++) x25519_fe_sq(r, r);
}

static void x25519_fe_add(x25519_fe *r, const x25519_fe *a, const x25519_fe *b) {
    for (int i = 0; i < 10; i++) {
        uint64_t v = (uint64_t)a->v[i] + (uint64_t)b->v[i];
        r->v[i] = (uint32_t)(v & X25519_LIMB_MASK);
    }
}

static void x25519_fe_sub(x25519_fe *r, const x25519_fe *a, const x25519_fe *b) {
    int64_t borrow = 0;
    for (int i = 0; i < 10; i++) {
        int64_t v = (int64_t)a->v[i] - (int64_t)b->v[i] - borrow;
        r->v[i] = (uint32_t)(v & X25519_LIMB_MASK);
        borrow = (v >> 63) & 1;
    }
}

static void x25519_fe_copy(x25519_fe *r, const x25519_fe *a) { memcpy(r, a, sizeof(*r)); }

static void x25519_core(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    x25519_fe x1, x2, z2, x3, z3;
    uint8_t swap = 0;
    x25519_fe_load(point, &x1);
    x25519_fe_set(&x2, 1);
    memset(&z2, 0, sizeof(z2));
    x25519_fe_copy(&x3, &x1);
    memset(&z3, 0, sizeof(z3));
    z3.v[0] = 1;
    for (int i = 255; i >= 0; i--) {
        uint8_t bit = (scalar[i / 8] >> (i % 8)) & 1;
        swap ^= bit;
        if (swap) {
            x25519_fe t; t = x2; x2 = x3; x3 = t; t = z2; z2 = z3; z3 = t;
        }
        swap = bit;
        x25519_fe t0, t1;
        x25519_fe_add(&t0, &x2, &z2);
        x25519_fe_sub(&t1, &x2, &z2);
        x25519_fe_sq(&x2, &t0);
        x25519_fe_sub(&z2, &x2, &t1);
        x25519_fe_sq(&z2, &z2);
        x25519_fe_mul(&z2, &z2, &x1);
        x25519_fe_sq(&t1, &t0);
        x25519_fe_add(&x3, &t1, &z2);
        x25519_fe_sub(&z3, &t1, &z2);
        x25519_fe_mul(&z3, &z3, &t0);
        x25519_fe_sq(&t0, &x3);
        x25519_fe_mul(&x3, &z3, &t1);
        x25519_fe_mul(&z3, &t0, &z2);
        x25519_fe_sq(&t1, &x2);
        x25519_fe_mul(&x2, &t1, &z2);
    }
    x25519_fe_mul(&z2, &z2, &z3);
    x25519_fe_inv(&z2, &z2);
    x25519_fe_mul(&x2, &x2, &z2);
    x25519_fe_store(&x2, out);
}

void x25519_keygen(uint8_t out[32]) {
    int i;
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(i * 17 + 42);
    out[0] &= 0xf8; out[31] &= 0x7f; out[31] |= 0x40;
}

void x25519_shared(uint8_t out[32], const uint8_t my_priv[32], const uint8_t peer_pub[32]) {
    uint8_t clamped[32];
    memcpy(clamped, my_priv, 32);
    clamped[0] &= 0xf8; clamped[31] &= 0x7f; clamped[31] |= 0x40;
    x25519_core(out, clamped, peer_pub);
}

#undef X25519_LIMB_BITS
#undef X25519_LIMB_MASK


