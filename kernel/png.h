#ifndef SOL_PNG_H
#define SOL_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Minimal PNG decoder for 8-bit, non-interlaced RGB(A) images with a
 * built-in DEFLATE (RFC 1951) inflater. Colour type 2 (RGB) and 6
 * (RGBA) at bit depth 8 are supported; everything else is rejected.
 * All memory (the decompression buffer and the result) is taken from
 * the kernel heap via kmalloc, so the caller must run after the heap
 * is up. The result is heap-allocated and owned by the caller (kfree
 * it when done).
 *
 * Returns 1 on success and stores into *out (0x00RRGGBB, row-major,
 * one 32-bit pixel per screen pixel), *w and *h. Returns 0 on any
 * malformed input or unsupported feature. The decoder never writes
 * outside the buffers it allocates: inflate output is capped to the
 * exact expected size and every read is bounds-checked. */
int png_decode(const void *data, size_t size,
               uint32_t **out, uint32_t *w, uint32_t *h);

#endif /* SOL_PNG_H */
