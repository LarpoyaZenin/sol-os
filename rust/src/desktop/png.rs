//! Self-contained PNG decoder with a built-in DEFLATE inflater.
//!
//! Port of `kernel/png.c`. Handles 8-bit RGB (color type 2) and RGBA
//! (color type 6), non-interlaced. The IDAT stream is inflated with a
//! from-scratch Huffman/DEFLATE decoder, then the per-row filters
//! (sub / up / average / paeth) are undone and pixels are packed as
//! `0x00RRGGBB` for the compositor. On the first row the "up",
//! "average" and "paeth" filters treat the previous row as all zeros,
//! as the spec requires.
//!
//! Everything is bounds-checked and heap-bounded; a corrupt file can
//! at worst produce a decode failure, never an out-of-bounds write.

use crate::memory::kheap;

const HUFF_MAX_BITS: u32 = 15;

/* ---- bit reader ---- */

struct BitReader<'a> {
    data: &'a [u8],
    pos: usize,
    bitbuf: u32,
    bitcnt: u32,
    err: bool,
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        BitReader {
            data,
            pos: 0,
            bitbuf: 0,
            bitcnt: 0,
            err: false,
        }
    }

    fn bits(&mut self, n: u32) -> u32 {
        while self.bitcnt < n {
            if self.pos >= self.data.len() {
                self.err = true;
                return 0;
            }
            self.bitbuf |= (self.data[self.pos] as u32) << self.bitcnt;
            self.pos += 1;
            self.bitcnt += 8;
        }
        let v = self.bitbuf & ((1u32 << n) - 1);
        self.bitbuf >>= n;
        self.bitcnt -= n;
        v
    }
}

/* ---- canonical Huffman tables ---- */

struct Huff {
    counts: [u16; (HUFF_MAX_BITS + 1) as usize],
    symbols: [u16; 320],
    nsym: usize,
}

impl Huff {
    fn new() -> Self {
        Huff {
            counts: [0; (HUFF_MAX_BITS + 1) as usize],
            symbols: [0; 320],
            nsym: 0,
        }
    }

    /// Builds a canonical table from a bit-length sequence.
    fn build(&mut self, lens: &[u8], n: usize) -> bool {
        for c in self.counts.iter_mut() {
            *c = 0;
        }
        self.nsym = 0;
        for i in 0..n {
            if lens[i] > HUFF_MAX_BITS as u8 {
                return false;
            }
            if lens[i] != 0 {
                self.counts[lens[i] as usize] += 1;
            }
        }
        for i in 1..=HUFF_MAX_BITS as usize {
            if self.counts[i] > (1u32 << i) as u16 {
                return false;
            }
        }
        let mut sym = 0usize;
        for len in 1..=HUFF_MAX_BITS as usize {
            for i in 0..n {
                if lens[i] == len as u8 && sym < self.symbols.len() {
                    self.symbols[sym] = i as u16;
                    sym += 1;
                }
            }
        }
        self.nsym = sym;
        true
    }

    /// Decodes one symbol; None on error.
    fn decode(&self, br: &mut BitReader) -> Option<u16> {
        let mut code: u32 = 0;
        let mut first: u32 = 0;
        let mut index: u32 = 0;
        for len in 1..=HUFF_MAX_BITS {
            code |= br.bits(1);
            if br.err {
                return None;
            }
            let cnt = self.counts[len as usize] as u32;
            if code < first + cnt {
                let sym = index + (code - first);
                if sym >= self.nsym as u32 {
                    return None;
                }
                return Some(self.symbols[sym as usize]);
            }
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        None
    }
}

/* ---- DEFLATE constants ---- */

const LEN_BASE: [u16; 29] = [
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131,
    163, 195, 227, 258,
];
const LEN_EXTRA: [u8; 29] = [
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
];
const DIST_BASE: [u16; 30] = [
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
    2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
];
const DIST_EXTRA: [u8; 30] = [
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13,
    13,
];
const CLC_ORDER: [u8; 19] = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];

fn huff_fixed(lit: &mut Huff, dist: &mut Huff) {
    let mut ll = [0u8; 288];
    let mut dd = [0u8; 32];
    for i in 0..144 {
        ll[i] = 8;
    }
    for i in 144..256 {
        ll[i] = 9;
    }
    for i in 256..280 {
        ll[i] = 7;
    }
    for i in 280..288 {
        ll[i] = 8;
    }
    for i in 0..32 {
        dd[i] = 5;
    }
    lit.build(&ll, 288);
    dist.build(&dd, 32);
}

/// Inflates `src` into the `out_cap`-byte buffer at `out`, returning
/// the number of bytes written, or None on error.
fn inflate(src: &[u8], out: *mut u8, out_cap: usize) -> Option<usize> {
    /* Skip a valid zlib wrapper if present (what real encoders emit
     * for IDAT): CMF low nibble = CM (8 = deflate), high nibble =
     * CINFO (<=7), FLG bit 5 = FDICT (0), header divisible by 31. */
    let mut data: &[u8] = src;
    if data.len() >= 2 {
        let cmf = data[0];
        let flg = data[1];
        if (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && (flg & 0x20) == 0
            && ((((cmf as u16) << 8) | flg as u16) % 31 == 0)
        {
            data = &data[2..];
        }
    }

    let mut br = BitReader::new(data);
    let mut opos = 0usize;
    let mut last = false;

    while !last {
        last = br.bits(1) != 0;
        let btype = br.bits(2);
        if br.err || btype == 3 {
            return None;
        }

        if btype == 0 {
            /* Stored block: byte-aligned LEN + NLEN, then literal bytes. */
            br.bitbuf = 0;
            br.bitcnt = 0;
            if br.pos + 4 > br.data.len() {
                return None;
            }
            let len = (br.data[br.pos] as usize) | ((br.data[br.pos + 1] as usize) << 8);
            let nlen = (br.data[br.pos + 2] as usize) | ((br.data[br.pos + 3] as usize) << 8);
            br.pos += 4;
            if (len ^ 0xFFFF) != nlen {
                return None;
            }
            if br.pos + len > br.data.len() {
                return None;
            }
            if opos + len > out_cap {
                return None;
            }
            unsafe {
                core::ptr::copy_nonoverlapping(br.data.as_ptr().add(br.pos), out.add(opos), len);
            }
            br.pos += len;
            opos += len;
            continue;
        }

        let mut lit = Huff::new();
        let mut dist = Huff::new();
        if btype == 1 {
            huff_fixed(&mut lit, &mut dist);
        } else {
            let hlit = 257 + br.bits(5) as usize;
            let hdist = 1 + br.bits(5) as usize;
            let hclen = 4 + br.bits(4) as usize;
            if br.err || hlit > 288 || hdist > 32 || hclen > 19 {
                return None;
            }
            let mut clc_lens = [0u8; 19];
            for i in 0..hclen {
                clc_lens[CLC_ORDER[i] as usize] = br.bits(3) as u8;
            }
            let mut clc = Huff::new();
            if br.err || !clc.build(&clc_lens, 19) {
                return None;
            }

            let mut lens = [0u8; 288 + 32];
            let total = hlit + hdist;
            let mut li = 0usize;
            while li < total {
                let sym = clc.decode(&mut br)?;
                if sym > 18 {
                    return None;
                }
                match sym {
                    0..=15 => {
                        lens[li] = sym as u8;
                        li += 1;
                    }
                    16 => {
                        if li == 0 {
                            return None;
                        }
                        let rep = 3 + br.bits(2) as usize;
                        let prev = lens[li - 1];
                        for _ in 0..rep {
                            if li >= total {
                                return None;
                            }
                            lens[li] = prev;
                            li += 1;
                        }
                    }
                    17 => {
                        let rep = 3 + br.bits(3) as usize;
                        for _ in 0..rep {
                            if li >= total {
                                return None;
                            }
                            lens[li] = 0;
                            li += 1;
                        }
                    }
                    _ => {
                        let rep = 11 + br.bits(7) as usize;
                        for _ in 0..rep {
                            if li >= total {
                                return None;
                            }
                            lens[li] = 0;
                            li += 1;
                        }
                    }
                }
            }
            if br.err || !lit.build(&lens, hlit) || !dist.build(&lens[hlit..], hdist) {
                return None;
            }
        }

        /* Literals / length-distance pairs until end-of-block. */
        loop {
            let sym = lit.decode(&mut br)?;
            if sym < 256 {
                if opos >= out_cap {
                    return None;
                }
                unsafe { *out.add(opos) = sym as u8 };
                opos += 1;
            } else if sym == 256 {
                break;
            } else if sym <= 285 {
                let li = (sym - 257) as usize;
                let len = LEN_BASE[li] as usize + br.bits(LEN_EXTRA[li] as u32) as usize;
                let dsym = dist.decode(&mut br)?;
                if dsym > 29 {
                    return None;
                }
                let distv = DIST_BASE[dsym as usize] as usize
                    + br.bits(DIST_EXTRA[dsym as usize] as u32) as usize;
                if br.err || distv > opos {
                    return None;
                }
                if opos + len > out_cap {
                    return None;
                }
                for i in 0..len {
                    unsafe { *out.add(opos + i) = *out.add(opos + i - distv) };
                }
                opos += len;
            } else {
                return None;
            }
        }
    }
    Some(opos)
}

fn be32(p: &[u8]) -> u32 {
    ((p[0] as u32) << 24) | ((p[1] as u32) << 16) | ((p[2] as u32) << 8) | (p[3] as u32)
}

/// Decodes a PNG. On success allocates a `w*h*4` heap buffer of
/// 0x00RRGGBB pixels, returns it (caller owns it via `kheap::kfree`).
pub fn decode(data: &[u8]) -> Option<(*mut u32, u32, u32)> {
    const SIG: [u8; 8] = [0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1A, b'\n'];
    if data.len() < 8 || data[0..8] != SIG {
        return None;
    }

    let mut width = 0u32;
    let mut height = 0u32;
    let mut bit_depth = 0u8;
    let mut color_type = 0u8;
    let mut idat: Option<(usize, usize)> = None; /* (start, end) */

    let mut pos = 8usize;
    while pos + 8 <= data.len() {
        let len = be32(&data[pos..]) as usize;
        let chunk = &data[pos + 8..];
        if pos + 8 + len > data.len() {
            return None;
        }
        let ty = &data[pos + 4..pos + 8];
        if ty == b"IHDR" {
            if len < 13 {
                return None;
            }
            width = be32(chunk);
            height = be32(&chunk[4..]);
            bit_depth = chunk[8];
            color_type = chunk[9];
            if chunk[10] != 0 || chunk[11] != 0 || chunk[12] != 0 {
                return None;
            }
            if width == 0 || height == 0 || width > 16384 || height > 16384 {
                return None;
            }
            if bit_depth != 8 || (color_type != 2 && color_type != 6) {
                return None;
            }
        } else if ty == b"IDAT" {
            if len > 0 {
                match idat {
                    None => idat = Some((pos + 8, pos + 8 + len)),
                    Some((s, e)) if s + e == pos + 8 => {
                        /* adjacent chunks: s..e followed directly */
                        let s = s;
                        let _ = e;
                        idat = Some((s, pos + 8 + len));
                    }
                    Some(_) => return None, /* non-contiguous IDAT unsupported */
                }
            }
        } else if ty == b"IEND" {
            break;
        }
        pos += 12 + len;
    }

    if width == 0 || height == 0 {
        return None;
    }
    let (idat_start, idat_end) = idat?;

    let channels = if color_type == 6 { 4 } else { 3 };
    let stride = (width as u64) * (channels as u64);
    let raw_size = (stride + 1) * (height as u64);
    if raw_size > 0x10000000 {
        return None; /* 256 MiB cap */
    }

    let raw = kheap::kmalloc(raw_size as usize);
    if raw.is_null() {
        return None;
    }

    let got = inflate(&data[idat_start..idat_end], raw, raw_size as usize);
    let got = match got {
        Some(g) if g == raw_size as usize => g,
        _ => {
            kheap::kfree(raw);
            return None;
        }
    };

    let img = kheap::kmalloc((width as usize) * (height as usize) * 4);
    if img.is_null() {
        kheap::kfree(raw);
        return None;
    }
    let img32 = img as *mut u32;

    for y in 0..height as usize {
        let row_off = y * (stride as usize + 1);
        let ft = unsafe { *raw.add(row_off) };
        let cur = unsafe { raw.add(row_off + 1) };
        let prev = if y == 0 {
            None
        } else {
            Some(unsafe { raw.add((y - 1) * (stride as usize + 1) + 1) })
        };

        match ft {
            1 => {
                for i in channels..stride as usize {
                    unsafe {
                        *cur.add(i) = cur.add(i).read().wrapping_add(cur.add(i - channels).read());
                    }
                }
            }
            2 => {
                for i in 0..stride as usize {
                    let b = unsafe { prev.map_or(0, |p| p.add(i).read()) };
                    unsafe {
                        *cur.add(i) = cur.add(i).read().wrapping_add(b);
                    }
                }
            }
            3 => {
                for i in 0..stride as usize {
                    let a = if i >= channels { unsafe { cur.add(i - channels).read() } } else { 0 };
                    let b = unsafe { prev.map_or(0, |p| p.add(i).read()) };
                    unsafe {
                        *cur.add(i) = cur.add(i).read().wrapping_add(((a as u16 + b as u16) >> 1) as u8);
                    }
                }
            }
            4 => {
                for i in 0..stride as usize {
                    let a = if i >= channels { unsafe { cur.add(i - channels).read() } } else { 0 };
                    let b = unsafe { prev.map_or(0, |p| p.add(i).read()) };
                    let c = if i >= channels {
                        unsafe { prev.map_or(0, |p| p.add(i - channels).read()) }
                    } else {
                        0
                    };
                    let p = (a as i32) + (b as i32) - (c as i32);
                    let pa = if p > a as i32 { p - a as i32 } else { a as i32 - p };
                    let pb = if p > b as i32 { p - b as i32 } else { b as i32 - p };
                    let pc = if p > c as i32 { p - c as i32 } else { c as i32 - p };
                    let pr = if pa <= pb && pa <= pc {
                        a
                    } else if pb <= pc {
                        b
                    } else {
                        c
                    };
                    unsafe {
                        *cur.add(i) = cur.add(i).read().wrapping_add(pr);
                    }
                }
            }
            0 => {}
            _ => {
                kheap::kfree(raw);
                kheap::kfree(img);
                return None;
            }
        }

        let row = unsafe { img32.add(y * width as usize) };
        for x in 0..width as usize {
            let px = unsafe { cur.add(x * channels) };
            unsafe {
                *row.add(x) = 0x0000_0000u32
                    | ((*px.add(0) as u32) << 16)
                    | ((*px.add(1) as u32) << 8)
                    | (*px.add(2) as u32);
            }
        }
    }

    kheap::kfree(raw);
    Some((img32, width, height))
}
