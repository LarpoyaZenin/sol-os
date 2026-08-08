#ifndef SOL_FONT_H
#define SOL_FONT_H

#include <stdint.h>

/* 8x8 monochrome bitmap font, public domain (based on IBM VGA 8x8
 * glyphs — see font.c header for attribution). One glyph per ASCII
 * code point U+0000..U+007F; each glyph is 8 rows of 8 pixels, MSB
 * of each row is the leftmost pixel. */
extern const uint8_t font8x8_basic[128][8];

#define FONT_W 8
#define FONT_H 8

#endif /* SOL_FONT_H */
