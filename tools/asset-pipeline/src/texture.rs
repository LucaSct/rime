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
use crate::PipelineError;

/// Wire format values (match `engine/assets/texture_asset.hpp`'s `TextureFormat`; append, never
/// renumber). RGBA8, tagged by *semantic*: colour data (baseColor/emissive) is sRGB; everything else
/// (normal/metallic-roughness/occlusion) is linear.
pub const TEXFMT_RGBA8_UNORM: u32 = 0; // linear data
pub const TEXFMT_RGBA8_SRGB: u32 = 1; // perceptual colour
                                      // 2 and 3 stay reserved for BC1/BC3. m16.7 makes these real; BC7 needs both colour spaces exactly
                                      // as RGBA8 does, so it takes two values.
pub const TEXFMT_BC5_UNORM: u32 = 4;
pub const TEXFMT_BC7_UNORM: u32 = 5;
pub const TEXFMT_BC7_SRGB: u32 = 6;

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
    /// The wire `format` value written into the payload header for an uncompressed texture.
    pub fn wire_format(self) -> u32 {
        match self {
            ColorSpace::Linear => TEXFMT_RGBA8_UNORM,
            ColorSpace::Srgb => TEXFMT_RGBA8_SRGB,
        }
    }

    /// The BC7 wire value for this colour space (m16.7).
    pub fn wire_format_bc7(self) -> u32 {
        match self {
            ColorSpace::Linear => TEXFMT_BC7_UNORM,
            ColorSpace::Srgb => TEXFMT_BC7_SRGB,
        }
    }
}

/// What a texture's ALPHA channel means, which decides how the cook may treat it (m19.7a). Cook-side
/// only — it never reaches the wire: an albedo+height texture is an ordinary `RGBA8_SRGB` texture to
/// the engine. Explicit discriminants anyway, appended and never renumbered, so that the day the cook
/// cache keys on usage the values are already stable.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum TextureUsage {
    /// Alpha is COVERAGE (or unused). Its mips get the m16.6 coverage rescale, so a cutout card does
    /// not dissolve at distance. Every texture cooked before m19.7a is this.
    #[default]
    Color = 0,
    /// Terrain layer albedo + height (M19.7a, ADR-0066): RGB = albedo (sRGB colour), A = height
    /// (LINEAR). The colour mips are gamma-correct exactly as for `Color`; the height mips are a
    /// plain average, and the coverage rescale is NOT applied — it would multiply a layer's relief
    /// up at distance, and a height map has no cutoff to preserve.
    TerrainAlbedoHeight = 1,
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
    /// What alpha means (coverage, or a terrain layer's height) — see [`TextureUsage`].
    pub usage: TextureUsage,
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
            usage: TextureUsage::Color,
            level0,
        }
    }

    /// Pack a terrain layer's albedo and height into ONE RGBA8 texture (M19.7a, ADR-0066): RGB from
    /// `albedo_rgba` (RGBA8; its alpha must be fully opaque and is replaced), A from `height` (one
    /// byte per texel, already 8-bit — see [`quantize_height_16_to_8`]). Both images must be the
    /// same size: resampling one to fit the other would invent detail or blur it away, and which one
    /// the author meant to be authoritative is not the cook's guess to make.
    ///
    /// Cooked as sRGB, which is right for both halves: Vulkan's `_SRGB` formats encode R, G and B
    /// only, so the sampler linearises the albedo and returns the height untouched.
    ///
    /// Uncompressed RGBA8 for now. BC7 (`cook_with(true)`) would work mechanically, but its mode-6
    /// encoder shares endpoint error between colour and alpha, and nobody has yet measured what that
    /// does to a height channel the blend sharpens — so it waits for a brick that measures it.
    pub fn terrain_albedo_height(
        (albedo_w, albedo_h): (u32, u32),
        albedo_rgba: &[u8],
        (height_w, height_h): (u32, u32),
        height: &[u8],
    ) -> Result<Self, PipelineError> {
        let bad = |msg: String| PipelineError::Unsupported(format!("terrain layer texture: {msg}"));
        if (albedo_w, albedo_h) != (height_w, height_h) {
            return Err(bad(format!(
                "albedo is {albedo_w}x{albedo_h} but height is {height_w}x{height_h}; they must match"
            )));
        }
        if albedo_w == 0 || albedo_h == 0 {
            return Err(bad("an empty image is not a layer".to_string()));
        }
        let texels = albedo_w as usize * albedo_h as usize;
        if albedo_rgba.len() != texels * BYTES_PER_PIXEL || height.len() != texels {
            return Err(bad(format!(
                "{} albedo bytes / {} height bytes for {albedo_w}x{albedo_h} texels",
                albedo_rgba.len(),
                height.len()
            )));
        }
        let mut level0 = Vec::with_capacity(texels * BYTES_PER_PIXEL);
        for (t, (rgba, &h)) in albedo_rgba
            .as_chunks::<BYTES_PER_PIXEL>()
            .0
            .iter()
            .zip(height)
            .enumerate()
        {
            // An albedo with real alpha is almost certainly a texture authored for something else
            // (or one where the author already put height in alpha, which this cook would silently
            // overwrite with the height image) — refuse rather than drop it.
            if rgba[3] != 255 {
                return Err(bad(format!(
                    "albedo texel ({}, {}) has alpha {} — the albedo must be opaque; its alpha \
                     channel is where the height goes",
                    t % albedo_w as usize,
                    t / albedo_w as usize,
                    rgba[3]
                )));
            }
            level0.extend_from_slice(&[rgba[0], rgba[1], rgba[2], h]);
        }
        Ok(Texture {
            width: albedo_w,
            height: albedo_h,
            color_space: ColorSpace::Srgb,
            usage: TextureUsage::TerrainAlbedoHeight,
            level0,
        })
    }

    /// Load a terrain layer's albedo (8-bit RGB or RGBA PNG/JPEG) and height (8- or 16-bit
    /// grayscale) images and pack them with [`Texture::terrain_albedo_height`]. A 16-bit height is
    /// quantised to 8 bits by [`quantize_height_16_to_8`]. Any other pixel format is refused: an
    /// implicit conversion (colour to grey, 16-bit colour to 8) is a decision the author should see.
    pub fn terrain_albedo_height_from_files(
        albedo: &Path,
        height: &Path,
    ) -> Result<Self, PipelineError> {
        let albedo_img = image::open(albedo).map_err(PipelineError::Image)?;
        let albedo_rgba = match albedo_img {
            image::DynamicImage::ImageRgb8(_) | image::DynamicImage::ImageRgba8(_) => {
                albedo_img.to_rgba8()
            }
            other => {
                return Err(PipelineError::Unsupported(format!(
                    "terrain layer albedo {}: expected 8-bit RGB or RGBA, found {:?}",
                    albedo.display(),
                    other.color()
                )))
            }
        };
        let (height_dims, height_bytes) = match image::open(height).map_err(PipelineError::Image)? {
            image::DynamicImage::ImageLuma8(buf) => (buf.dimensions(), buf.into_raw()),
            image::DynamicImage::ImageLuma16(buf) => (
                buf.dimensions(),
                buf.into_raw()
                    .into_iter()
                    .map(quantize_height_16_to_8)
                    .collect(),
            ),
            other => {
                return Err(PipelineError::Unsupported(format!(
                    "terrain layer height {}: expected 8- or 16-bit grayscale, found {:?}",
                    height.display(),
                    other.color()
                )))
            }
        };
        Texture::terrain_albedo_height(
            albedo_rgba.dimensions(),
            albedo_rgba.as_raw(),
            height_dims,
            &height_bytes,
        )
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
        // Coverage is the only alpha this rescale is right for; a height channel keeps its plain
        // average (see TextureUsage::TerrainAlbedoHeight).
        if self.usage == TextureUsage::Color {
            preserve_alpha_coverage(&mut chain);
        }
        chain
    }

    /// Encode this texture into a complete RMA1 file, returning `(bytes, asset_id)`. The layout
    /// mirrors `decode_texture` in the engine's reader exactly: a fixed header, the mip table, then
    /// every level's pixels concatenated.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        self.cook_with(false)
    }

    /// Cook, optionally BLOCK-COMPRESSING every level as BC7 (m16.7).
    ///
    /// BC7 rather than BC5 for now, even for normal maps: BC5 stores two channels and the shader
    /// would have to reconstruct Z, which `pbr_forward_shadowed.frag` does not do yet (it reads z
    /// from the texture). BC7 is the zero-shader-churn option, and ADR-0039 records BC5-for-normals
    /// as the follow-on once that shader change lands. The encoder for both already exists in
    /// `bcn`, so that brick is a selection change rather than new compression work.
    pub fn cook_with(&self, block_compress: bool) -> (Vec<u8>, u64) {
        let mut mips = self.generate_mips();
        if block_compress {
            for m in &mut mips {
                m.pixels = crate::bcn::compress_level(&m.pixels, m.width, m.height, false);
            }
        }

        let mut p = ByteWriter::new();
        p.u32(self.width);
        p.u32(self.height);
        p.u32(if block_compress {
            self.color_space.wire_format_bc7()
        } else {
            self.color_space.wire_format()
        });
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

/// Quantise a 16-bit height sample to 8 bits, ROUND-TO-NEAREST (m19.7a): `round(v * 255 / 65535)`,
/// which is `round(v / 257)`, in integers as `(2v + 257) / 514`. There are no ties to break: a tie
/// would need `2v = 257 * (2k + 1)`, an odd number, and `2v` is even. Truncating (`v >> 8`) instead
/// would bias every height down by half a step on average (256, 99.6 % of the way to 1, becomes 0)
/// — a skew the height blend, which compares heights ACROSS layers, would turn into a systematic
/// preference for whichever layer was authored at 8 bits.
pub fn quantize_height_16_to_8(v: u16) -> u8 {
    ((2 * u32::from(v) + 257) / 514) as u8
}

/// The reference cutoff coverage is preserved against (m16.6).
///
/// Coverage is a per-MATERIAL property (each material has its own `alpha_cutoff`) applied to a
/// per-TEXTURE cook, and textures dedup across materials — one image can be the base colour of
/// several materials with different cutoffs, and they cannot all be right. Two ways out, and the
/// one not taken is worth recording: putting the cutoff into the texture's cook key would
/// un-dedup, cooking the same image once per distinct cutoff and silently multiplying asset size.
///
/// So the chain is scaled against glTF's DEFAULT cutoff of 0.5. A material that masks at 0.5 —
/// which is the default and the overwhelming majority — gets exact coverage; one that masks
/// elsewhere gets a chain that is still far better than the unscaled box filter, but not exact.
/// The limitation is documented rather than hidden, and the cook counts the case (see
/// `coverage_mismatch_count`).
const COVERAGE_REFERENCE_CUTOFF: f32 = 0.5;

/// The most alpha may be scaled up in pursuit of coverage.
///
/// A cap is a QUALITY choice, not a numerical convenience. Once a level is blurred enough that its
/// feature has dissolved, the only way to restore the level-0 coverage is to multiply a faint haze
/// into something solid — which trades one artefact (the card thinning out) for a worse one (a
/// distant card reading as an opaque blob). So coverage is preserved while the level can still
/// carry it, and past that the chain degrades gracefully instead of lying.
const MAX_COVERAGE_SCALE: f32 = 4.0;

/// Rescale each mip level's alpha so the FRACTION OF TEXELS ABOVE THE CUTOFF matches level 0.
///
/// A box filter averages coverage away: a leaf card that is 40% opaque at level 0 drifts toward the
/// mean at every level, so at distance the alpha test rejects almost everything and the card
/// vanishes. Authors compensate by over-dilating masks and dropping the cutoff, which is a
/// workaround for a cook bug.
///
/// The fix is the standard one: for each level, binary-search a scale factor for alpha such that
/// the post-scale coverage matches level 0's. Scaling (not thresholding) keeps the level usable for
/// anything that samples alpha smoothly; only the coverage is pinned.
///
/// Levels with no partial alpha at all — every texel fully opaque or fully transparent — are left
/// alone: there is nothing to preserve and a scale would be arbitrary.
fn preserve_alpha_coverage(chain: &mut [MipLevel]) {
    if chain.is_empty() {
        return;
    }
    let target = coverage(&chain[0], 1.0);
    if target <= 0.0 || target >= 1.0 {
        return; // fully transparent or fully opaque at level 0: nothing to preserve
    }
    for level in chain.iter_mut().skip(1) {
        // Binary search the alpha scale. 16 iterations over [0, 4] lands within ~1e-4, far finer
        // than the 1/255 the stored byte can express.
        let (mut lo, mut hi) = (0.0f32, MAX_COVERAGE_SCALE);
        for _ in 0..16 {
            let mid = 0.5 * (lo + hi);
            if coverage(level, mid) < target {
                lo = mid; // too transparent: scale up
            } else {
                hi = mid;
            }
        }
        // `hi`, NOT the midpoint. The search maintains coverage(lo) < target <= coverage(hi), and
        // coverage is a STEP function of the scale (there are finitely many texels), so a midpoint
        // between the two can land on the low side of the step and undershoot the target. Taking
        // `hi` is the only choice the invariant actually guarantees.
        let scale = hi;
        for texel in level.pixels.as_chunks_mut::<BYTES_PER_PIXEL>().0 {
            let a = f32::from(texel[3]) / 255.0;
            texel[3] = ((a * scale) * 255.0).round().clamp(0.0, 255.0) as u8;
        }
    }
}

/// The fraction of texels whose alpha, scaled by `scale`, is at or above the reference cutoff.
fn coverage(level: &MipLevel, scale: f32) -> f32 {
    let mut above = 0usize;
    let total = level.pixels.len() / BYTES_PER_PIXEL;
    if total == 0 {
        return 0.0;
    }
    for texel in level.pixels.as_chunks::<BYTES_PER_PIXEL>().0 {
        if (f32::from(texel[3]) / 255.0) * scale >= COVERAGE_REFERENCE_CUTOFF {
            above += 1;
        }
    }
    above as f32 / total as f32
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
    // m16.6 — alpha coverage across the mip chain.
    //
    // A box filter averages coverage away, so a masked card drifts toward the mean at every level
    // and the alpha test rejects almost everything at distance: the card vanishes. Authors
    // compensate with over-dilated masks and a lowered cutoff, which is a workaround for a cook bug.
    //
    // The test asserts coverage stays within 2% of level 0, and its NEGATIVE CONTROL runs the
    // identical assertion against the unscaled chain in the same test. That control is what proves
    // the tolerance is tight enough to have caught the old behaviour — without it, a 2% window
    // might simply be wide enough to accept the drift it exists to reject.
    #[test]
    fn mip_alpha_preserves_coverage_and_the_tolerance_would_have_caught_the_old_behaviour() {
        // A sparse, smooth, APERIODIC alpha field — foliage-shaped in the ways that matter here.
        //
        // Three properties the fixture needs, and they pull against each other. HIGH FREQUENCY, or
        // a box filter destroys nothing and there is no bug to catch. SMOOTH, so each level carries
        // many distinct alpha values — coverage is a step function of the scale factor otherwise,
        // and the target falls between steps. And SPARSE (coverage well under half), because a
        // field symmetric about the cutoff keeps its coverage under blurring for free.
        //
        // An earlier attempt used a grid of soft discs at `x % 16`, which failed a property that is
        // easy to miss: every cell was identical, so an integer grid produced only ~11 distinct
        // radii and therefore ~11 distinct alphas. Measured, not guessed. Incommensurate
        // frequencies avoid that — no two texels share a value by construction.
        const N: u32 = 128;
        let mut px = vec![0u8; (N * N) as usize * BYTES_PER_PIXEL];
        for y in 0..N {
            for x in 0..N {
                let i = ((y * N + x) as usize) * BYTES_PER_PIXEL;
                let fx = x as f32 * 0.61;
                let fy = y as f32 * 0.47;
                let lobe = (fx.sin() * fy.sin()).max(0.0);
                let a = (lobe * lobe * lobe * 255.0).clamp(0.0, 255.0);
                px[i] = 255;
                px[i + 1] = 255;
                px[i + 2] = 255;
                px[i + 3] = a as u8;
            }
        }
        let tex = Texture::from_rgba8(N, N, ColorSpace::Srgb, px);

        // The plain box chain, which is what the cook produced before this brick.
        let mut unscaled = vec![MipLevel {
            width: tex.width,
            height: tex.height,
            pixels: tex.level0.clone(),
        }];
        while unscaled.last().unwrap().width > 1 || unscaled.last().unwrap().height > 1 {
            let prev = unscaled.last().unwrap();
            unscaled.push(downsample(prev, ColorSpace::Srgb));
        }

        let scaled = tex.generate_mips();
        assert_eq!(scaled.len(), unscaled.len());

        let level0 = coverage(&scaled[0], 1.0);
        // Partial is the only property that matters: a fully-opaque or fully-transparent fixture
        // has no coverage to preserve.
        assert!(
            level0 > 0.05 && level0 < 0.9,
            "fixture coverage should be partial, got {level0}"
        );

        // COVERAGE CAN ONLY BE PRESERVED WHERE A LEVEL CAN EXPRESS IT, and that is a real limit
        // rather than a convenience. Once alpha has blurred to a handful of distinct values,
        // coverage is a STEP function of the scale and the target falls between steps — at 2x2 the
        // only representable coverages are quarters.
        //
        // Resolvability is judged on the UNSCALED level, not the scaled one: scaling multiplies
        // alpha and clamps at 255, which collapses distinct values on purpose, so asking the scaled
        // level how expressive it is would be asking after the fact.
        let distinct_alphas = |l: &MipLevel| -> usize {
            let mut seen = [false; 256];
            let mut n = 0;
            for t in l.pixels.as_chunks::<BYTES_PER_PIXEL>().0 {
                if !seen[t[3] as usize] {
                    seen[t[3] as usize] = true;
                    n += 1;
                }
            }
            n
        };

        let mut checked = 0;
        let mut drifted = false;
        for i in 0..scaled.len() {
            if distinct_alphas(&unscaled[i]) < 8 {
                continue;
            }
            // Levels the SCALE CAP deliberately declines to fix: past the point where even the
            // maximum scale cannot reach level 0's coverage, the feature has dissolved and the
            // cook chooses graceful degradation over inflating haze into a solid blob. Asserting
            // there would be asserting against a documented design choice.
            if coverage(&unscaled[i], MAX_COVERAGE_SCALE) < level0 {
                continue;
            }
            checked += 1;
            let c = coverage(&scaled[i], 1.0);
            assert!(
                (c - level0).abs() <= 0.02,
                "level {i} ({}x{}) coverage {c} drifted from {level0}",
                scaled[i].width,
                scaled[i].height
            );
            if (coverage(&unscaled[i], 1.0) - level0).abs() > 0.02 {
                drifted = true;
            }
        }
        assert!(
            checked >= 2,
            "only {checked} levels were testable — fixture too small"
        );

        // THE CONTROL: over exactly the same levels, the unscaled chain must FAIL the assertion the
        // scaled chain just passed. Otherwise the 2% window is merely wide, not tight.
        assert!(
            drifted,
            "the unscaled chain must drift, or this test could not have caught the old behaviour"
        );
    }

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

    // ── m19.7a: terrain albedo + height packing ─────────────────────────────────────────────────

    #[test]
    fn terrain_pack_takes_rgb_from_albedo_and_a_from_height_exactly() {
        // A 2x2 where every byte is distinct, so a swapped channel, a transposed walk or a dropped
        // texel each produce different bytes.
        let albedo = [
            10, 20, 30, 255, /**/ 11, 21, 31, 255, //
            12, 22, 32, 255, /**/ 13, 23, 33, 255,
        ];
        let height = [200, 201, 202, 203];
        let tex = Texture::terrain_albedo_height((2, 2), &albedo, (2, 2), &height).unwrap();
        assert_eq!(
            tex.level0,
            vec![
                10, 20, 30, 200, /**/ 11, 21, 31, 201, //
                12, 22, 32, 202, /**/ 13, 23, 33, 203,
            ]
        );
        assert_eq!((tex.width, tex.height), (2, 2));
        assert_eq!(tex.color_space, ColorSpace::Srgb);
        assert_eq!(tex.usage, TextureUsage::TerrainAlbedoHeight);
        // And it lands on the wire as an ordinary sRGB RGBA8 texture (format field is the 3rd u32).
        let (bytes, _) = tex.cook();
        let (_, payload) = crate::cooked::read_header(&bytes).unwrap();
        assert_eq!(
            u32::from_le_bytes(payload[8..12].try_into().unwrap()),
            TEXFMT_RGBA8_SRGB
        );
    }

    #[test]
    fn terrain_pack_refuses_mismatched_sizes_and_non_opaque_albedo() {
        let albedo_2x2 = [255u8; 16];
        let err = Texture::terrain_albedo_height((2, 2), &albedo_2x2, (2, 1), &[0, 0]).unwrap_err();
        assert!(err.to_string().contains("must match"), "{err}");
        let err = Texture::terrain_albedo_height((2, 2), &albedo_2x2, (1, 2), &[0, 0]).unwrap_err();
        assert!(err.to_string().contains("must match"), "{err}");
        let mut translucent = albedo_2x2;
        translucent[7] = 254; // texel (1, 0)'s alpha
        let err =
            Texture::terrain_albedo_height((2, 2), &translucent, (2, 2), &[0; 4]).unwrap_err();
        assert!(err.to_string().contains("(1, 0)"), "{err}");
    }

    #[test]
    fn sixteen_bit_height_rounds_to_nearest() {
        // Each pair brackets a rounding boundary: v/257 just below and just above k + 0.5.
        let cases: [(u16, u8); 14] = [
            (0, 0),
            (128, 0), // 0.498
            (129, 1), // 0.502 — floor would say 0
            (256, 1), // 0.996 — `v >> 8` would say 0
            (257, 1),
            (385, 1), // 1.498
            (386, 2), // 1.502
            (32767, 127),
            (32768, 128),
            (65406, 254), // 254.498
            (65407, 255), // 254.502
            (65535, 255),
            (1000, 4),
            (50000, 195),
        ];
        for (v, q) in cases {
            assert_eq!(quantize_height_16_to_8(v), q, "v = {v}");
            // Against the real-number definition too.
            assert_eq!(f64::from(v) * 255.0 / 65535.0, f64::from(v) / 257.0);
            assert_eq!((f64::from(v) / 257.0).round() as u8, q, "v = {v}");
        }
        // Monotone and onto: every 8-bit level is reachable.
        let mut seen = [false; 256];
        let mut prev = 0u8;
        for v in 0..=u16::MAX {
            let q = quantize_height_16_to_8(v);
            assert!(q >= prev);
            prev = q;
            seen[q as usize] = true;
        }
        assert!(seen.iter().all(|&b| b));
    }

    #[test]
    fn terrain_height_mips_are_a_plain_average_not_coverage_rescaled() {
        // A height field with partial alpha everywhere — exactly what makes the m16.6 coverage
        // rescale fire for a Color texture. For a terrain layer the mip's alpha must be the plain
        // box average of the four heights below it.
        let albedo = [128u8, 128, 128, 255].repeat(4);
        let height = [10u8, 30, 200, 250];
        let terrain = Texture::terrain_albedo_height((2, 2), &albedo, (2, 2), &height).unwrap();
        let mips = terrain.generate_mips();
        assert_eq!(mips.len(), 2);
        let mean = (10.0f32 + 30.0 + 200.0 + 250.0) / 4.0; // 122.5
        assert_eq!(mips[1].pixels[3], mean.round() as u8);
        // The negative control: the same bytes cooked as a Color texture ARE rescaled, so the
        // assertion above would have caught the rescale leaking into the terrain path.
        let mut color = terrain.clone();
        color.usage = TextureUsage::Color;
        assert_ne!(color.generate_mips()[1].pixels[3], mean.round() as u8);
        // Colour stays gamma-correct either way (flat grey in, flat grey out).
        assert_eq!(&mips[1].pixels[0..3], &[128, 128, 128]);
    }

    #[test]
    fn terrain_texture_cook_is_byte_stable() {
        let albedo: Vec<u8> = (0..64u8)
            .map(|i| if i % 4 == 3 { 255 } else { i * 3 })
            .collect();
        let height: Vec<u8> = (0..16u8).map(|i| i * 17).collect();
        let cook = || {
            Texture::terrain_albedo_height((4, 4), &albedo, (4, 4), &height)
                .unwrap()
                .cook()
        };
        assert_eq!(cook(), cook());
    }
}
