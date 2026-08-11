#include "png.h"
#include "mm/kheap.h"
#include <stddef.h>
#include <stdint.h>

/* PNG decoder with a self-contained DEFLATE inflater.
 *
 * The format on disk is a sequence of length-prefixed chunks; the
 * image data lives in one or more IDAT chunks, compressed with
 * DEFLATE. We inflate the IDAT stream into the exact size of the
 * filtered image (one filter byte + `stride` bytes per row), then
 * undo the per-row filters (sub / up / average / paeth) and convert
 * the RGB(A) bytes into the 0x00RRGGBB pixels the rest of the
 * graphics stack uses.
 *
 * Everything is bounds-checked and heap-bounded; a corrupt file can
 * at worst produce a decode failure, never an out-of-bounds write. */

/* ---- bit reader ---- */

struct br {
    const uint8_t *p;
    size_t n;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
};

static uint32_t br_bits(struct br *b, int n) {
    while (b->bitcnt < n) {
        if (b->pos >= b->n) return 0xFFFFFFFFu;
        b->bitbuf |= (uint32_t)b->p[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    uint32_t v = b->bitbuf & ((1u << n) - 1u);
    b->bitbuf >>= n;
    b->bitcnt -= n;
    return v;
}

/* ---- Huffman decoding (canonical codes) ---- */

#define HUFF_MAX_BITS 15

struct huff {
    uint16_t counts[HUFF_MAX_BITS + 1];  /* number of codes of each length */
    uint16_t symbols[288 + 32];          /* symbols ordered by (len, symbol) */
    int nsym;
};

/* Decode one symbol. Returns -1 on error. */
static int huff_dec(struct huff *h, struct br *b) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        code |= (int)br_bits(b, 1);
        int cnt = h->counts[len];
        if (code - cnt < first) {
            int sym = index + (code - first);
            if (sym < 0 || sym >= h->nsym) return -1;
            return h->symbols[sym];
        }
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    return -1;
}

/* Builds a canonical Huffman table from a bit-length sequence. */
static int huff_build(struct huff *h, const uint8_t *lens, int n) {
    for (int i = 0; i <= HUFF_MAX_BITS; i++) h->counts[i] = 0;
    h->nsym = 0;
    for (int i = 0; i < n; i++) {
        if (lens[i] > HUFF_MAX_BITS) return 0;
        if (lens[i] != 0) h->counts[lens[i]]++;
    }
    /* Deflate allows at most one code of each length for distances. */
    for (int i = 1; i <= HUFF_MAX_BITS; i++) {
        if (h->counts[i] > (1u << i)) return 0;
    }
    int sym = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        for (int i = 0; i < n; i++) {
            if (lens[i] == len && sym < (int)(sizeof(h->symbols) / sizeof(h->symbols[0]))) {
                h->symbols[sym++] = (uint16_t)i;
            }
        }
    }
    h->nsym = sym;
    return 1;
}

/* ---- DEFLATE constants ---- */

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577,
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

/* Builds the fixed litlen/dist tables from DEFLATE's spec. */
static void huff_fixed(struct huff *lit, struct huff *dist) {
    uint8_t ll[288];
    uint8_t dd[32];
    for (int i = 0; i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0; i < 32; i++) dd[i] = 5;
    huff_build(lit, ll, 288);
    huff_build(dist, dd, 32);
}

/* The 19 code-length code order. */
static const uint8_t clc_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

/* Inflates `src`/`n` into `out`/`out_cap`; returns the number of
 * bytes written, or (size_t)-1 on error. out_cap is never exceeded. */
static size_t inflate(const uint8_t *src, size_t n,
                      uint8_t *out, size_t out_cap) {
    /* Optional zlib wrapper (what real-world encoders emit for IDAT):
     * CMF low nibble = CM (8 = deflate), high nibble = CINFO (<=7),
     * FLG bit 5 = FDICT (0), and the 16-bit header is a multiple of 31.
     * When those hold, skip the 2-byte header into the DEFLATE stream. */
    if (n >= 2) {
        uint8_t cmf = src[0], flg = src[1];
        if ((cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && !(flg & 0x20) &&
            ((((uint16_t)cmf << 8) | flg) % 31 == 0)) {
            src += 2;
            n -= 2;
        }
    }
    struct br b;
    b.p = src;
    b.n = n;
    b.pos = 0;
    b.bitbuf = 0;
    b.bitcnt = 0;
    size_t opos = 0;
    int last = 0;

    while (!last) {
        last = (int)br_bits(&b, 1);
        int type = (int)br_bits(&b, 2);
        if (type == 3) return (size_t)-1;

        if (type == 0) {
            /* Stored block: byte-aligned, LEN + NLEN, literal bytes. */
            b.bitbuf = 0;
            b.bitcnt = 0;
            if (b.pos + 4 > b.n) return (size_t)-1;
            size_t len = (size_t)b.p[b.pos] | ((size_t)b.p[b.pos + 1] << 8);
            size_t nlen = (size_t)b.p[b.pos + 2] | ((size_t)b.p[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFF) != nlen) return (size_t)-1;
            if (b.pos + len > b.n) return (size_t)-1;
            if (opos + len > out_cap) return (size_t)-1;
            for (size_t i = 0; i < len; i++) out[opos++] = b.p[b.pos + i];
            b.pos += len;
            continue;
        }

        struct huff lit, dist;
        if (type == 1) {
            huff_fixed(&lit, &dist);
        } else {
            int hlit = 257 + (int)br_bits(&b, 5);
            int hdist = 1 + (int)br_bits(&b, 5);
            int hclen = 4 + (int)br_bits(&b, 4);
            if (hlit > 288 || hdist > 32 || hclen > 19) return (size_t)-1;
            uint8_t clc_lens[19];
            for (int i = 0; i < 19; i++) clc_lens[i] = 0;
            for (int i = 0; i < hclen; i++) clc_lens[clc_order[i]] = (uint8_t)br_bits(&b, 3);
            struct huff clc;
            if (!huff_build(&clc, clc_lens, 19)) return (size_t)-1;

            uint8_t lens[288 + 32];
            int total = hlit + hdist;
            int li = 0;
            while (li < total) {
                int sym = huff_dec(&clc, &b);
                if (sym < 0 || sym > 18) return (size_t)-1;
                if (sym < 16) {
                    lens[li++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (li == 0) return (size_t)-1;
                    int rep = 3 + (int)br_bits(&b, 2);
                    uint8_t prev = lens[li - 1];
                    while (rep-- > 0) {
                        if (li >= total) return (size_t)-1;
                        lens[li++] = prev;
                    }
                } else if (sym == 17) {
                    int rep = 3 + (int)br_bits(&b, 3);
                    while (rep-- > 0) {
                        if (li >= total) return (size_t)-1;
                        lens[li++] = 0;
                    }
                } else {
                    int rep = 11 + (int)br_bits(&b, 7);
                    while (rep-- > 0) {
                        if (li >= total) return (size_t)-1;
                        lens[li++] = 0;
                    }
                }
            }
            if (!huff_build(&lit, lens, hlit)) return (size_t)-1;
            if (!huff_build(&dist, lens + hlit, hdist)) return (size_t)-1;
        }

        /* Decode literals / matches until the end-of-block marker. */
        for (;;) {
            int sym = huff_dec(&lit, &b);
            if (sym < 0) return (size_t)-1;
            if (sym < 256) {
                if (opos >= out_cap) return (size_t)-1;
                out[opos++] = (uint8_t)sym;
            } else if (sym == 256) {
                break;
            } else if (sym <= 285) {
                int li = sym - 257;
                size_t len = len_base[li] + (size_t)br_bits(&b, len_extra[li]);
                int dsym = huff_dec(&dist, &b);
                if (dsym < 0 || dsym > 29) return (size_t)-1;
                size_t distv = dist_base[dsym] + (size_t)br_bits(&b, dist_extra[dsym]);
                if (distv > opos) return (size_t)-1;
                if (opos + len > out_cap) return (size_t)-1;
                for (size_t i = 0; i < len; i++) out[opos] = out[opos - distv], opos++;
            } else {
                return (size_t)-1;
            }
        }
    }
    return opos;
}

/* ---- PNG chunk walking ---- */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int png_decode(const void *data, size_t size,
               uint32_t **out, uint32_t *w, uint32_t *h) {
    if (out == NULL || w == NULL || h == NULL) return 0;
    *out = NULL;
    *w = 0;
    *h = 0;

    const uint8_t *p = (const uint8_t *)data;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (size < 8) return 0;
    for (int i = 0; i < 8; i++) {
        if (p[i] != sig[i]) return 0;
    }

    uint32_t width = 0, height = 0, bit_depth = 0, color_type = 0;
    const uint8_t *idat = NULL;
    size_t idat_len = 0;

    size_t pos = 8;
    while (pos + 8 <= size) {
        uint32_t len = be32(p + pos);
        const uint8_t *type = p + pos + 4;
        const uint8_t *chunk = p + pos + 8;
        if (pos + 8 + len > size) return 0;

        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            if (len < 13) return 0;
            width  = be32(chunk + 0);
            height = be32(chunk + 4);
            bit_depth = chunk[8];
            color_type = chunk[9];
            /* compression method 0, filter method 0, interlace 0 */
            if (chunk[10] != 0 || chunk[11] != 0 || chunk[12] != 0) return 0;
            if (width == 0 || height == 0 || width > 16384 || height > 16384) return 0;
            if (bit_depth != 8) return 0;
            if (color_type != 2 && color_type != 6) return 0;
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (len > 0) {
                if (idat == NULL) {
                    idat = chunk;
                    idat_len = len;
                } else if (idat + idat_len == chunk) {
                    idat_len += len;   /* adjacent IDAT chunks */
                } else {
                    return 0;          /* non-contiguous IDAT not supported */
                }
            }
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }

        pos += 12 + len;
    }

    if (width == 0 || height == 0 || idat == NULL) return 0;

    int channels = (color_type == 6) ? 4 : 3;
    uint64_t stride = (uint64_t)width * (uint64_t)channels;
    uint64_t raw_size = (stride + 1) * (uint64_t)height;
    if (raw_size > 0x10000000u) return 0;   /* 256 MiB cap */

    uint8_t *raw = (uint8_t *)kmalloc((size_t)raw_size);
    if (raw == NULL) return 0;

    size_t got = inflate(idat, idat_len, raw, (size_t)raw_size);
    if (got != raw_size) {
        kfree(raw);
        return 0;
    }

    uint32_t *img = (uint32_t *)kmalloc((size_t)width * (size_t)height * 4u);
    if (img == NULL) {
        kfree(raw);
        return 0;
    }

    for (uint32_t y = 0; y < height; y++) {
        uint8_t ft = raw[y * (stride + 1)];
        uint8_t *cur = raw + y * (stride + 1) + 1;
        const uint8_t *prev = (y == 0) ? NULL : raw + (y - 1) * (stride + 1) + 1;

        if (ft == 1) {
            for (uint64_t i = channels; i < stride; i++)
                cur[i] = (uint8_t)(cur[i] + cur[i - channels]);
        } else if (ft == 2) {
            if (prev == NULL) return 0;   /* filter 2 on the first row is invalid */
            for (uint64_t i = 0; i < stride; i++)
                cur[i] = (uint8_t)(cur[i] + prev[i]);
        } else if (ft == 3) {
            for (uint64_t i = 0; i < stride; i++) {
                uint8_t a = (i >= channels) ? cur[i - channels] : 0;
                uint8_t b = (prev == NULL) ? 0 : prev[i];
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
        } else if (ft == 4) {
            for (uint64_t i = 0; i < stride; i++) {
                uint8_t a = (i >= channels) ? cur[i - channels] : 0;
                uint8_t b = (prev == NULL) ? 0 : prev[i];
                uint8_t c = (i >= channels && prev != NULL) ? prev[i - channels] : 0;
                int p = (int)a + (int)b - (int)c;
                int pa = p > a ? p - a : a - p;
                int pb = p > b ? p - b : b - p;
                int pc = p > c ? p - c : c - p;
                uint8_t pr;
                if (pa <= pb && pa <= pc) pr = a;
                else if (pb <= pc) pr = b;
                else pr = c;
                cur[i] = (uint8_t)(cur[i] + pr);
            }
        } else if (ft != 0) {
            kfree(img);
            kfree(raw);
            return 0;
        }

        uint32_t *row = img + (uint64_t)y * width;
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *px = cur + (uint64_t)x * channels;
            row[x] = 0x00000000u | ((uint32_t)px[0] << 16) |
                     ((uint32_t)px[1] << 8) | (uint32_t)px[2];
        }
    }

    kfree(raw);
    *out = img;
    *w = width;
    *h = height;
    return 1;
}
