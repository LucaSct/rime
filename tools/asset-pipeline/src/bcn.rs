// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Block compression: BC5 for two-channel data, BC7 for RGBA (m16.7, ADR-0039).
//!
//! # Why this is written here rather than pulled in
//!
//! `third_party/README.md` is explicit: "we don't pull in a library for something we should
//! understand and own", and the directory vendors no dependency *source* because sources are
//! fetched and checksummed at build time. The obvious BC7 crate wraps Intel's ISPC compressor via
//! **prebuilt per-platform binaries** — a vendored binary on a three-OS CI, for a format that is a
//! few hundred lines to encode. So both encoders live here, and the decoders alongside them,
//! because a compressor you cannot decode is a compressor you cannot test.
//!
//! # What these formats buy
//!
//! Fixed-rate, GPU-native compression: the hardware samples the compressed bytes directly, so the
//! saving is in memory AND bandwidth, permanently — unlike a PNG, which is only small on disk.
//! BC7 is 8 bits per texel (a quarter of RGBA8); BC5 is 8 bits per texel for two channels (half of
//! RG8, and the natural normal-map format since a tangent-space normal's Z can be reconstructed).
//!
//! # The quality limitation, stated up front
//!
//! **BC7 here writes mode 6 only.** BC7 has eight modes with partitioning schemes for blocks whose
//! texels do not lie near one line in RGBA space; a full encoder searches them. Mode 6 is the
//! single-subset, full-precision RGBA mode — excellent on smooth blocks and mediocre on
//! high-contrast ones (a hard black/white edge inside one 4x4 block is where it shows). That is a
//! deliberate first cut: it is correct, it is a quarter the size, and the seam for a mode search is
//! this module. The test's error bound documents what the current encoder actually achieves rather
//! than what BC7 could achieve.

/// One 4x4 block of RGBA8 texels, row-major.
pub type Block = [[u8; 4]; 16];

// ── BC4 / BC5 ────────────────────────────────────────────────────────────────────────────────

/// Encode one channel of a 4x4 block as a BC4 block (8 bytes).
///
/// BC4 stores two 8-bit endpoints and sixteen 3-bit indices into a palette built from them. When
/// `r0 > r1` the palette is eight evenly-spaced values; the other ordering reserves two slots for
/// exact 0 and 255 at the cost of coarser spacing. We always take the eight-value form, because a
/// data channel (roughness, a normal's X) has no special meaning at the extremes that would repay
/// two of its eight levels.
fn encode_bc4_block(values: &[u8; 16]) -> [u8; 8] {
    let mut lo = 255u8;
    let mut hi = 0u8;
    for &v in values {
        lo = lo.min(v);
        hi = hi.max(v);
    }

    // A flat block: both endpoints equal, every index 0. Writing r0 == r1 would select the
    // six-value palette, which still decodes r0 at index 0 — so this is exact either way.
    if lo == hi {
        return [hi, lo, 0, 0, 0, 0, 0, 0];
    }

    let palette = bc4_palette(hi, lo);
    let mut bits: u64 = 0;
    for (i, &v) in values.iter().enumerate() {
        let mut best = 0usize;
        let mut best_err = i32::MAX;
        for (idx, &p) in palette.iter().enumerate() {
            let err = (i32::from(v) - i32::from(p)).abs();
            if err < best_err {
                best_err = err;
                best = idx;
            }
        }
        bits |= (best as u64) << (3 * i);
    }

    let mut out = [0u8; 8];
    out[0] = hi; // r0 > r1 selects the eight-value palette
    out[1] = lo;
    for i in 0..6 {
        out[2 + i] = ((bits >> (8 * i)) & 0xFF) as u8;
    }
    out
}

/// The eight-entry BC4 palette for `r0 > r1`, in index order.
fn bc4_palette(r0: u8, r1: u8) -> [u8; 8] {
    let a = i32::from(r0);
    let b = i32::from(r1);
    let mut p = [0u8; 8];
    p[0] = r0;
    p[1] = r1;
    for (i, slot) in p.iter_mut().enumerate().skip(2) {
        let w = i as i32;
        // The BC4 rule: index i (2..=7) is ((8-i)*r0 + (i-1)*r1) / 7.
        *slot = (((8 - w) * a + (w - 1) * b) / 7) as u8;
    }
    p
}

fn decode_bc4_block(bytes: &[u8; 8]) -> [u8; 16] {
    let r0 = bytes[0];
    let r1 = bytes[1];
    let palette = if r0 > r1 {
        bc4_palette(r0, r1)
    } else {
        // The six-value form, which our encoder only emits for a flat block.
        let a = i32::from(r0);
        let b = i32::from(r1);
        let mut p = [0u8; 8];
        p[0] = r0;
        p[1] = r1;
        for (i, slot) in p.iter_mut().enumerate().take(6).skip(2) {
            let w = i as i32;
            *slot = (((6 - w) * a + (w - 1) * b) / 5) as u8;
        }
        p[6] = 0;
        p[7] = 255;
        p
    };
    let mut bits: u64 = 0;
    for i in 0..6 {
        bits |= u64::from(bytes[2 + i]) << (8 * i);
    }
    let mut out = [0u8; 16];
    for (i, o) in out.iter_mut().enumerate() {
        *o = palette[((bits >> (3 * i)) & 0x7) as usize];
    }
    out
}

/// Encode a 4x4 block as BC5 (16 bytes): two independent BC4 blocks, R then G.
pub fn encode_bc5_block(block: &Block) -> [u8; 16] {
    let mut r = [0u8; 16];
    let mut g = [0u8; 16];
    for i in 0..16 {
        r[i] = block[i][0];
        g[i] = block[i][1];
    }
    let rb = encode_bc4_block(&r);
    let gb = encode_bc4_block(&g);
    let mut out = [0u8; 16];
    out[..8].copy_from_slice(&rb);
    out[8..].copy_from_slice(&gb);
    out
}

/// Decode a BC5 block. Blue is 0 and alpha 255: BC5 carries two channels, and a consumer that
/// wants a normal's Z reconstructs it (z = sqrt(1 - x² - y²)) rather than reading it.
pub fn decode_bc5_block(bytes: &[u8; 16]) -> Block {
    let rb: [u8; 8] = bytes[..8].try_into().unwrap();
    let gb: [u8; 8] = bytes[8..].try_into().unwrap();
    let r = decode_bc4_block(&rb);
    let g = decode_bc4_block(&gb);
    let mut out = [[0u8; 4]; 16];
    for i in 0..16 {
        out[i] = [r[i], g[i], 0, 255];
    }
    out
}

// ── BC7, mode 6 ──────────────────────────────────────────────────────────────────────────────

/// The 4-bit interpolation weights BC7 uses (the spec's `aWeight4`).
const BC7_WEIGHTS4: [i32; 16] = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];

/// Interpolate two 8-bit endpoints at a 4-bit weight, per the BC7 rule.
fn bc7_lerp(e0: u8, e1: u8, w: i32) -> u8 {
    (((64 - w) * i32::from(e0) + w * i32::from(e1) + 32) >> 6) as u8
}

/// Encode a 4x4 block as BC7 mode 6 (16 bytes).
///
/// Mode 6 is one subset spanning the whole block: two RGBA endpoints at 7 bits per channel plus a
/// shared p-bit each (giving 8 bits of effective precision), and sixteen 4-bit indices. The layout
/// is 7 mode bits, 56 endpoint bits, 2 p-bits and 63 index bits — the first index drops its high
/// bit, which is what the endpoint swap below exists to satisfy.
pub fn encode_bc7_mode6_block(block: &Block) -> [u8; 16] {
    // Endpoints: the per-channel extremes of the block. A least-squares fit along the principal
    // axis would do better on blocks whose texels are not axis-aligned in RGBA space; the bounding
    // box is the honest first cut and is exact for a flat or single-gradient block.
    let mut lo = [255u8; 4];
    let mut hi = [0u8; 4];
    for texel in block {
        for c in 0..4 {
            lo[c] = lo[c].min(texel[c]);
            hi[c] = hi[c].max(texel[c]);
        }
    }

    // Endpoints are stored as 7 bits + one shared p-bit. Round each to the nearest representable
    // 8-bit value of the form (v7 << 1) | p, choosing the p-bit that costs least over the block.
    let mut best: Option<(u32, [u8; 16])> = None;
    for p0 in 0..2u8 {
        for p1 in 0..2u8 {
            let e0 = quantise_endpoint(lo, p0);
            let e1 = quantise_endpoint(hi, p1);
            let (err, bytes) = pack_mode6(block, e0, e1, p0, p1);
            if best.as_ref().is_none_or(|(b, _)| err < *b) {
                best = Some((err, bytes));
            }
        }
    }
    best.expect("the p-bit loop always runs").1
}

/// Round an 8-bit endpoint to the nearest value representable as `(v7 << 1) | p`, returning the
/// DECODED 8-bit value the hardware will reconstruct (which is what the index search must target).
fn quantise_endpoint(v: [u8; 4], p: u8) -> [u8; 4] {
    let mut out = [0u8; 4];
    for c in 0..4 {
        // v7 is 7 bits; the reconstructed value is (v7 << 1) | p.
        let target = i32::from(v[c]);
        let v7 = (((target - i32::from(p)) + 1) / 2).clamp(0, 127);
        out[c] = ((v7 << 1) | i32::from(p)) as u8;
    }
    out
}

/// Pack one candidate mode-6 block and report its squared error against the source.
fn pack_mode6(block: &Block, e0: [u8; 4], e1: [u8; 4], p0: u8, p1: u8) -> (u32, [u8; 16]) {
    // Build the 16-entry palette once, then pick each texel's nearest entry.
    let mut palette = [[0u8; 4]; 16];
    for (i, entry) in palette.iter_mut().enumerate() {
        for c in 0..4 {
            entry[c] = bc7_lerp(e0[c], e1[c], BC7_WEIGHTS4[i]);
        }
    }

    let mut indices = [0u8; 16];
    let mut error = 0u32;
    for (t, texel) in block.iter().enumerate() {
        let mut best = 0usize;
        let mut best_err = u32::MAX;
        for (i, entry) in palette.iter().enumerate() {
            let mut err = 0u32;
            for c in 0..4 {
                let d = i32::from(texel[c]) - i32::from(entry[c]);
                err += (d * d) as u32;
            }
            if err < best_err {
                best_err = err;
                best = i;
            }
        }
        indices[t] = best as u8;
        error += best_err;
    }

    // THE ANCHOR RULE: index 0 is stored in 3 bits, so its high bit must be zero. If it is not,
    // swap the endpoints and invert every index — which describes the identical palette traversed
    // in the opposite direction, so the decoded block is unchanged.
    let (e0, e1, p0, p1, indices) = if indices[0] & 0x8 != 0 {
        let mut inv = [0u8; 16];
        for (dst, &src) in inv.iter_mut().zip(indices.iter()) {
            *dst = 15 - src;
        }
        (e1, e0, p1, p0, inv)
    } else {
        (e0, e1, p0, p1, indices)
    };

    let mut bits = BitWriter::default();
    bits.put(1 << 6, 7); // mode 6: six zeros then a one, in the low seven bits
    for c in 0..4 {
        bits.put(u32::from(e0[c] >> 1), 7);
        bits.put(u32::from(e1[c] >> 1), 7);
    }
    bits.put(u32::from(p0), 1);
    bits.put(u32::from(p1), 1);
    bits.put(u32::from(indices[0]), 3); // the anchor: high bit implicit
    for &idx in &indices[1..] {
        bits.put(u32::from(idx), 4);
    }
    (error, bits.finish())
}

/// Decode a BC7 mode-6 block. Only mode 6 is understood — this exists to test the encoder, not to
/// be a general BC7 decoder, and it says so by refusing anything else.
pub fn decode_bc7_mode6_block(bytes: &[u8; 16]) -> Block {
    let mut r = BitReader::new(bytes);
    let mode = r.get(7);
    assert_eq!(mode, 1 << 6, "decode_bc7_mode6_block: not a mode-6 block");

    let mut e0 = [0u8; 4];
    let mut e1 = [0u8; 4];
    for c in 0..4 {
        e0[c] = r.get(7) as u8;
        e1[c] = r.get(7) as u8;
    }
    let p0 = r.get(1) as u8;
    let p1 = r.get(1) as u8;
    for c in 0..4 {
        e0[c] = (e0[c] << 1) | p0;
        e1[c] = (e1[c] << 1) | p1;
    }

    let mut indices = [0u8; 16];
    indices[0] = r.get(3) as u8;
    for idx in indices.iter_mut().skip(1) {
        *idx = r.get(4) as u8;
    }

    let mut out = [[0u8; 4]; 16];
    for (t, texel) in out.iter_mut().enumerate() {
        let w = BC7_WEIGHTS4[indices[t] as usize];
        for c in 0..4 {
            texel[c] = bc7_lerp(e0[c], e1[c], w);
        }
    }
    out
}

// ── Bit plumbing ─────────────────────────────────────────────────────────────────────────────

#[derive(Default)]
struct BitWriter {
    lo: u64,
    hi: u64,
    at: u32,
}

impl BitWriter {
    fn put(&mut self, value: u32, bits: u32) {
        for i in 0..bits {
            let bit = u64::from((value >> i) & 1);
            if self.at < 64 {
                self.lo |= bit << self.at;
            } else {
                self.hi |= bit << (self.at - 64);
            }
            self.at += 1;
        }
    }

    fn finish(self) -> [u8; 16] {
        let mut out = [0u8; 16];
        out[..8].copy_from_slice(&self.lo.to_le_bytes());
        out[8..].copy_from_slice(&self.hi.to_le_bytes());
        out
    }
}

struct BitReader {
    lo: u64,
    hi: u64,
    at: u32,
}

impl BitReader {
    fn new(bytes: &[u8; 16]) -> Self {
        Self {
            lo: u64::from_le_bytes(bytes[..8].try_into().unwrap()),
            hi: u64::from_le_bytes(bytes[8..].try_into().unwrap()),
            at: 0,
        }
    }

    fn get(&mut self, bits: u32) -> u32 {
        let mut v = 0u32;
        for i in 0..bits {
            let bit = if self.at < 64 {
                (self.lo >> self.at) & 1
            } else {
                (self.hi >> (self.at - 64)) & 1
            };
            v |= (bit as u32) << i;
            self.at += 1;
        }
        v
    }
}

// ── Whole-image encoding ─────────────────────────────────────────────────────────────────────

/// Gather the 4x4 block whose top-left texel is at (bx*4, by*4), clamping reads at the image edge
/// so a non-multiple-of-4 extent still produces whole blocks (the format's own rule).
fn gather_block(pixels: &[u8], width: u32, height: u32, bx: u32, by: u32) -> Block {
    let mut block = [[0u8; 4]; 16];
    for ty in 0..4u32 {
        for tx in 0..4u32 {
            let x = (bx * 4 + tx).min(width - 1) as usize;
            let y = (by * 4 + ty).min(height - 1) as usize;
            let i = (y * width as usize + x) * 4;
            block[(ty * 4 + tx) as usize] =
                [pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]];
        }
    }
    block
}

/// Compress a whole RGBA8 level. `bc5` selects the two-channel format; otherwise BC7 mode 6.
pub fn compress_level(pixels: &[u8], width: u32, height: u32, bc5: bool) -> Vec<u8> {
    let bw = width.div_ceil(4);
    let bh = height.div_ceil(4);
    let mut out = Vec::with_capacity((bw * bh) as usize * 16);
    for by in 0..bh {
        for bx in 0..bw {
            let block = gather_block(pixels, width, height, bx, by);
            let bytes = if bc5 {
                encode_bc5_block(&block)
            } else {
                encode_bc7_mode6_block(&block)
            };
            out.extend_from_slice(&bytes);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Mean absolute error per channel between a source image and its round trip.
    fn round_trip_error(pixels: &[u8], width: u32, height: u32, bc5: bool) -> f32 {
        let compressed = compress_level(pixels, width, height, bc5);
        let bw = width.div_ceil(4);
        let mut total = 0u64;
        let mut count = 0u64;
        for by in 0..height.div_ceil(4) {
            for bx in 0..bw {
                let off = ((by * bw + bx) as usize) * 16;
                let bytes: [u8; 16] = compressed[off..off + 16].try_into().unwrap();
                let decoded = if bc5 {
                    decode_bc5_block(&bytes)
                } else {
                    decode_bc7_mode6_block(&bytes)
                };
                let source = gather_block(pixels, width, height, bx, by);
                for t in 0..16 {
                    // BC5 carries two channels; comparing its zeroed B/A would measure the format's
                    // definition rather than the encoder's accuracy.
                    let channels = if bc5 { 2 } else { 4 };
                    for c in 0..channels {
                        total += i32::from(source[t][c]).abs_diff(i32::from(decoded[t][c])) as u64;
                        count += 1;
                    }
                }
            }
        }
        total as f32 / count as f32
    }

    /// A smooth gradient with some structure — the case block compression is good at, and the one
    /// a wrong bit layout would still fail catastrophically.
    fn gradient(width: u32, height: u32) -> Vec<u8> {
        let mut px = vec![0u8; (width * height) as usize * 4];
        for y in 0..height {
            for x in 0..width {
                let i = ((y * width + x) as usize) * 4;
                px[i] = (x * 255 / width.max(1)) as u8;
                px[i + 1] = (y * 255 / height.max(1)) as u8;
                px[i + 2] = ((x + y) * 255 / (width + height).max(1)) as u8;
                px[i + 3] = 255;
            }
        }
        px
    }

    #[test]
    fn bc7_round_trips_a_gradient_within_a_bound() {
        let px = gradient(64, 64);
        let err = round_trip_error(&px, 64, 64, false);
        // The bound documents what THIS encoder achieves (mode 6, bounding-box endpoints), not what
        // BC7 could achieve with a mode search. A wrong bit layout does not land near it — it lands
        // in the tens.
        assert!(
            err < 3.0,
            "BC7 mean abs error {err} exceeds the documented bound"
        );
    }

    #[test]
    fn bc5_round_trips_two_channels_within_a_bound() {
        let px = gradient(64, 64);
        let err = round_trip_error(&px, 64, 64, true);
        assert!(
            err < 3.0,
            "BC5 mean abs error {err} exceeds the documented bound"
        );
    }

    #[test]
    fn a_flat_block_survives_within_the_formats_own_precision() {
        // A flat block is the case where any error is pure quantisation, so it pins the endpoint
        // handling exactly.
        //
        // BC5 is EXACT: BC4 endpoints are full 8-bit, so a constant channel round-trips bit for
        // bit. BC7 mode 6 is exact only to within ONE, and that is the format rather than the
        // encoder: its endpoints are 7 bits plus a SHARED p-bit, so all four channels of an
        // endpoint must agree in parity to be represented exactly. [37, 211, 90, 255] deliberately
        // does not — three odd values and one even — so the best any mode-6 encoder can do is miss
        // by one on the odd-one-out.
        let block: Block = [[37, 211, 90, 255]; 16];

        let bc5 = decode_bc5_block(&encode_bc5_block(&block));
        for t in 0..16 {
            assert_eq!(
                &bc5[t][..2],
                &block[t][..2],
                "BC5 must be exact on a flat block (texel {t})"
            );
        }

        let bc7 = decode_bc7_mode6_block(&encode_bc7_mode6_block(&block));
        for t in 0..16 {
            for c in 0..4 {
                let d = i32::from(bc7[t][c]).abs_diff(i32::from(block[t][c]));
                assert!(d <= 1, "BC7 flat block off by {d} at texel {t} channel {c}");
            }
        }
        // …and a block whose channels DO share parity must be exact, which is what proves the
        // tolerance above is the format's limit and not the encoder being sloppy.
        let even: Block = [[36, 210, 90, 254]; 16];
        let exact = decode_bc7_mode6_block(&encode_bc7_mode6_block(&even));
        for t in 0..16 {
            assert_eq!(
                exact[t], even[t],
                "BC7 must be exact when the parities agree (texel {t})"
            );
        }
    }

    #[test]
    fn compressed_size_is_the_promised_fraction_and_rounds_up_to_whole_blocks() {
        // 8 bits per texel — one byte per texel, a quarter of RGBA8's four.
        assert_eq!(
            compress_level(&gradient(64, 64), 64, 64, false).len(),
            64 * 64
        );
        assert_eq!(
            compress_level(&gradient(64, 64), 64, 64, false).len() * 4,
            64 * 64 * 4
        );
        // …and a 5x5 image is 2x2 blocks, not "1.25 squared" — the rule the reader's size maths
        // has to agree with.
        assert_eq!(compress_level(&gradient(5, 5), 5, 5, false).len(), 4 * 16);
        assert_eq!(compress_level(&gradient(1, 1), 1, 1, true).len(), 16);
    }

    #[test]
    fn the_error_bound_rejects_a_block_decoded_as_the_wrong_format() {
        // THE CONTROL. Without it the bound above proves only that some number is small — it must
        // also be tight enough to REJECT a wrong decode. BC7 bytes read as BC5 are two BC4 blocks
        // of nonsense, and the error must blow through the bound.
        let px = gradient(64, 64);
        let compressed = compress_level(&px, 64, 64, false); // BC7
        let bytes: [u8; 16] = compressed[..16].try_into().unwrap();
        let wrong = decode_bc5_block(&bytes); // …decoded as BC5
        let source = gather_block(&px, 64, 64, 0, 0);
        let mut total = 0u64;
        for t in 0..16 {
            for c in 0..2 {
                total += i32::from(source[t][c]).abs_diff(i32::from(wrong[t][c])) as u64;
            }
        }
        let err = total as f32 / 32.0;
        assert!(
            err > 3.0,
            "decoding BC7 as BC5 produced error {err}, which the bound would accept"
        );
    }
}
