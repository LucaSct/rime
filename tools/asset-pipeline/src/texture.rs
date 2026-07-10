// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The cooker's in-memory texture and its RMA1 texture-payload encoder (M6.3). A source PNG/JPEG is
//! decoded to RGBA8, an **offline mip chain** is generated, and the whole thing is written in the
//! byte layout `engine/assets`' `decode_texture` validates (see `docs/design/assets.md`).
//!
//! The one technique here worth reading twice is **gamma-correct mip generation**. A colour texture
//! is stored sRGB-encoded — perceptually uniform, *not* proportional to light. Averaging sRGB bytes
//! directly (the tempting one-liner) averages the wrong quantity and makes every minified surface too
//! dark — the classic "dark mipmaps" bug. So for an sRGB texture we *linearise* each texel to light,
//! average in linear space, then *re-encode* to sRGB. A black/white checker's coarse mip is then
//! sRGB ~188 (linear 0.5 re-encoded), the correct mid-grey — not 128, the too-dark naive average.
//! A linear texture (normal maps, metallic-roughness, occlusion — data, not colour) is averaged
//! directly, because its bytes already *are* proportional to the quantity we want to filter.

use std::path::Path;

use crate::cooked::{wrap_container, ByteWriter, ASSET_KIND_TEXTURE, TEXTURE_SCHEMA_HASH};

/// Wire format values (match `engine/assets/texture_asset.hpp`'s `TextureFormat`; append, never
/// renumber). RGBA8, tagged by *semantic*: colour data (baseColor/emissive) is sRGB; everything else
/// (normal/metallic-roughness/occlusion) is linear.
pub const TEXFMT_RGBA8_UNORM: u32 = 0; // linear data
pub const TEXFMT_RGBA8_SRGB: u32 = 1; // perceptual colour

/// Four bytes per texel — the one place the RGBA8 stride lives (mirrors `kTextureBytesPerPixel`).
pub const BYTES_PER_PIXEL: usize = 4;

/// How a texture's bytes relate to light, which decides how its mips are filtered. `Srgb` bytes are
/// perceptual (must be linearised before averaging); `Linear` bytes are the quantity itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ColorSpace {
    Linear,
    Srgb,
}

impl ColorSpace {
    /// The wire `format` value written into the payload header.
    pub fn wire_format(self) -> u32 {
        match self {
            ColorSpace::Linear => TEXFMT_RGBA8_UNORM,
            ColorSpace::Srgb => TEXFMT_RGBA8_SRGB,
        }
    }
}

/// One generated mip level: its extent and its RGBA8 pixels (row-major, top row first).
#[derive(Debug, Clone)]
pub struct MipLevel {
    pub width: u32,
    pub height: u32,
    pub pixels: Vec<u8>,
}

/// A cook-ready texture: RGBA8 level 0 plus the colour space that governs mip filtering. The full
/// chain is generated at `cook()` time.
#[derive(Debug, Clone)]
pub struct Texture {
    pub width: u32,
    pub height: u32,
    pub color_space: ColorSpace,
    /// Level-0 pixels, RGBA8, row-major, `width * height * 4` bytes.
    pub level0: Vec<u8>,
}

impl Texture {
    /// Wrap raw RGBA8 level-0 pixels (the unit-testable entry point). Panics on a size mismatch —
    /// this is cooker-internal, not a file boundary.
    pub fn from_rgba8(width: u32, height: u32, color_space: ColorSpace, level0: Vec<u8>) -> Self {
        assert_eq!(
            level0.len(),
            width as usize * height as usize * BYTES_PER_PIXEL,
            "level-0 pixel count must be width*height*4"
        );
        Texture {
            width,
            height,
            color_space,
            level0,
        }
    }

    /// Decode a PNG/JPEG file into an RGBA8 level-0 texture. No vertical flip is applied: the `image`
    /// crate yields row 0 as the *top* of the source image, which is already the engine's UV
    /// convention — uv (0,0) samples the top-left texel (Vulkan's top-left origin; what the M3.5
    /// textured-quad proof asserts and what `write_texture` uploads). Flipping here would turn every
    /// cooked texture upside-down relative to the sampler.
    pub fn from_file(path: &Path, color_space: ColorSpace) -> Result<Self, image::ImageError> {
        let rgba = image::open(path)?.to_rgba8();
        let (width, height) = rgba.dimensions();
        Ok(Texture::from_rgba8(
            width,
            height,
            color_space,
            rgba.into_raw(),
        ))
    }

    /// Generate the full mip chain, level 0 first. Each subsequent level is the previous one box-
    /// filtered to half size (floored, min 1) — gamma-correctly for an sRGB texture (see the module
    /// note). The chain ends at 1×1, giving `floor(log2(max(w,h))) + 1` levels, exactly the count and
    /// per-level extents the engine's `full_mip_count` / `mip_extent` expect.
    pub fn generate_mips(&self) -> Vec<MipLevel> {
        let mut chain = vec![MipLevel {
            width: self.width,
            height: self.height,
            pixels: self.level0.clone(),
        }];
        while chain.last().unwrap().width > 1 || chain.last().unwrap().height > 1 {
            let prev = chain.last().unwrap();
            chain.push(downsample(prev, self.color_space));
        }
        chain
    }

    /// Encode this texture into a complete RMA1 file, returning `(bytes, asset_id)`. The layout
    /// mirrors `decode_texture` in the engine's reader exactly: a fixed header, the mip table, then
    /// every level's pixels concatenated.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let mips = self.generate_mips();

        let mut p = ByteWriter::new();
        p.u32(self.width);
        p.u32(self.height);
        p.u32(self.color_space.wire_format());
        p.u32(mips.len() as u32);

        // The mip table: {width, height, offset, size} per level, offsets tiling the pixel blob that
        // follows. This is the record the schema hash fingerprints.
        let mut offset: u32 = 0;
        for m in &mips {
            let size = m.pixels.len() as u32;
            p.u32(m.width);
            p.u32(m.height);
            p.u32(offset);
            p.u32(size);
            offset += size;
        }
        // The pixel blob: levels concatenated in the same order as the table.
        for m in &mips {
            p.bytes(&m.pixels);
        }

        wrap_container(ASSET_KIND_TEXTURE, TEXTURE_SCHEMA_HASH, &p.into_vec())
    }
}

/// Box-filter one mip level to the next (half width/height, floored, min 1). Each destination texel
/// averages the 2×2 source block above it, in *light* — for sRGB that means decode → average →
/// re-encode; for linear, a plain average. Alpha is always linear (it is coverage, never gamma-
/// encoded), so it is averaged directly regardless of colour space. Source indices are clamped to the
/// level's edge, which makes the filter degrade gracefully on an odd or 1-wide dimension (the 2×2
/// window collapses to the samples that exist) rather than reading out of bounds.
///
/// Two v1 policy choices, documented per the brick's kickoff notes: (1) a plain **box** filter — the
/// quality seam for a triangle/Kaiser kernel is here, adopted only if a measured need appears; and (2)
/// **straight (non-premultiplied) alpha** — colour and alpha are averaged independently, so a texture
/// authored straight round-trips exactly. Premultiplied alpha (which avoids colour bleeding from fully
/// transparent texels) is a later option, gated on a texture that actually needs it.
fn downsample(src: &MipLevel, color_space: ColorSpace) -> MipLevel {
    let dst_w = (src.width / 2).max(1);
    let dst_h = (src.height / 2).max(1);
    let mut pixels = vec![0u8; dst_w as usize * dst_h as usize * BYTES_PER_PIXEL];

    let srgb = color_space == ColorSpace::Srgb;
    let sample = |x: u32, y: u32, c: usize| -> f32 {
        let xi = x.min(src.width - 1) as usize;
        let yi = y.min(src.height - 1) as usize;
        let byte = src.pixels[(yi * src.width as usize + xi) * BYTES_PER_PIXEL + c];
        // Colour channels (0..3) are linearised for an sRGB texture; alpha (3) never is.
        if srgb && c < 3 {
            srgb_to_linear(byte)
        } else {
            byte as f32 / 255.0
        }
    };

    for y2 in 0..dst_h {
        for x2 in 0..dst_w {
            let (sx, sy) = (x2 * 2, y2 * 2);
            for c in 0..BYTES_PER_PIXEL {
                let sum = sample(sx, sy, c)
                    + sample(sx + 1, sy, c)
                    + sample(sx, sy + 1, c)
                    + sample(sx + 1, sy + 1, c);
                let avg = sum / 4.0;
                let byte = if srgb && c < 3 {
                    linear_to_srgb(avg)
                } else {
                    (avg * 255.0).round().clamp(0.0, 255.0) as u8
                };
                pixels[(y2 as usize * dst_w as usize + x2 as usize) * BYTES_PER_PIXEL + c] = byte;
            }
        }
    }

    MipLevel {
        width: dst_w,
        height: dst_h,
        pixels,
    }
}

/// sRGB → linear light (the standard sRGB EOTF), for a single 0..255 channel. The piecewise curve is
/// the IEC 61966-2-1 definition: a short linear toe near black, a 2.4-power segment above it.
fn srgb_to_linear(byte: u8) -> f32 {
    let c = byte as f32 / 255.0;
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

/// Linear light → sRGB (the inverse curve), rounded to a 0..255 channel. Exact inverse of
/// `srgb_to_linear`, so a decode/encode round-trip of any byte is the identity.
fn linear_to_srgb(l: f32) -> u8 {
    let s = if l <= 0.003_130_8 {
        12.92 * l
    } else {
        1.055 * l.powf(1.0 / 2.4) - 0.055
    };
    (s * 255.0).round().clamp(0.0, 255.0) as u8
}

#[cfg(test)]
mod tests {
    use super::*;

    // A 2×2 checker of pure black and white texels (opaque), the canonical gamma test image.
    fn checker_2x2() -> Vec<u8> {
        let black = [0u8, 0, 0, 255];
        let white = [255u8, 255, 255, 255];
        let mut px = Vec::new();
        px.extend_from_slice(&white); // (0,0)
        px.extend_from_slice(&black); // (1,0)
        px.extend_from_slice(&black); // (0,1)
        px.extend_from_slice(&white); // (1,1)
        px
    }

    #[test]
    fn srgb_transfer_round_trips_every_byte() {
        for b in 0u16..=255 {
            let b = b as u8;
            assert_eq!(
                linear_to_srgb(srgb_to_linear(b)),
                b,
                "sRGB round trip failed at {b}"
            );
        }
    }

    #[test]
    fn gamma_correct_mip_of_an_srgb_checker_is_light_grey_not_dark() {
        // sRGB: linearise (black→0, white→1), average (→0.5 linear), re-encode → sRGB 188. This is
        // the correct mid-grey. The naive "average the sRGB bytes" bug would give 128 — too dark.
        let tex = Texture::from_rgba8(2, 2, ColorSpace::Srgb, checker_2x2());
        let mips = tex.generate_mips();
        assert_eq!(mips.len(), 2); // 2×2 → 1×1
        let mip1 = &mips[1];
        assert_eq!((mip1.width, mip1.height), (1, 1));
        assert_eq!(
            mip1.pixels,
            vec![188, 188, 188, 255],
            "gamma-correct sRGB average must be ~188 (linear 0.5 re-encoded), not 128"
        );
        assert_ne!(
            mip1.pixels[0], 128,
            "128 would be the gamma-wrong naive average"
        );
    }

    #[test]
    fn plain_mip_of_a_linear_checker_is_the_direct_average() {
        // Linear data (e.g. a normal/MR/AO map): the bytes already *are* the quantity, so a coarse
        // mip is their plain average — 0.5 → 128. Applying the sRGB round-trip here would be the
        // *inverse* bug (too light), which this pins against.
        let tex = Texture::from_rgba8(2, 2, ColorSpace::Linear, checker_2x2());
        let mip1 = &tex.generate_mips()[1];
        assert_eq!(mip1.pixels, vec![128, 128, 128, 255]);
    }

    #[test]
    fn mip_dimensions_halve_down_to_one_by_one() {
        // A non-square texture: 4×2 → 2×1 → 1×1 (3 levels), each dimension halving with a floor at 1.
        let tex = Texture::from_rgba8(4, 2, ColorSpace::Srgb, vec![200u8; 4 * 2 * 4]);
        let dims: Vec<(u32, u32)> = tex
            .generate_mips()
            .iter()
            .map(|m| (m.width, m.height))
            .collect();
        assert_eq!(dims, vec![(4, 2), (2, 1), (1, 1)]);

        // And a square power-of-two: 8×8 → … → 1×1 is 4 levels.
        let sq = Texture::from_rgba8(8, 8, ColorSpace::Linear, vec![0u8; 8 * 8 * 4]);
        assert_eq!(sq.generate_mips().len(), 4);
    }

    #[test]
    fn a_flat_colour_survives_downsampling_unchanged() {
        // Every texel identical → every mip texel identical (both colour spaces): the filter has
        // nothing to average away. Guards against an off-by-one or a stray gamma shift on flat input.
        for cs in [ColorSpace::Srgb, ColorSpace::Linear] {
            let tex = Texture::from_rgba8(4, 4, cs, vec![120u8; 4 * 4 * 4]);
            for m in tex.generate_mips() {
                assert!(
                    m.pixels.iter().all(|&b| b == 120),
                    "flat colour drifted under {cs:?}"
                );
            }
        }
    }

    #[test]
    fn cook_is_byte_stable_and_carries_the_texture_kind() {
        use crate::cooked::{read_header, ASSET_KIND_TEXTURE, TEXTURE_SCHEMA_HASH};
        let tex = Texture::from_rgba8(4, 4, ColorSpace::Srgb, vec![64u8; 4 * 4 * 4]);
        let (a, id_a) = tex.cook();
        let (b, id_b) = tex.cook();
        assert_eq!(a, b, "cook must be deterministic");
        assert_eq!(id_a, id_b);

        let (header, _payload) = read_header(&a).unwrap();
        assert_eq!(header.asset_kind, ASSET_KIND_TEXTURE);
        assert_eq!(header.type_schema_hash, TEXTURE_SCHEMA_HASH);
    }
}
