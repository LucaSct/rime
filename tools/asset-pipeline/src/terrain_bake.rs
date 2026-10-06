// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The terrain **appearance bake** (M19.8d3, ADR-0072): what a coarse LOD tile looks like.
//!
//! A level-0 tile is shaded from its splat map and its layers' textures (ADR-0063/0066). A parent
//! tile (ADR-0070) covers 2^L × 2^L of them, which may carry different palettes — their union
//! need not fit four layers — and at the distance a parent is drawn the layer textures are far
//! below a pixel anyway. So the cook evaluates the level-0 appearance once, offline, and stores
//! its average per parent as two small textures the renderer samples instead of blending layers:
//!
//! * **base colour**, RGBA8 sRGB (A = 255);
//! * **material**, RGBA8 UNORM: R = metallic, G = roughness (B = 0, A = 255).
//!
//! Two textures rather than one packed RGBA8: the colour wants the sRGB transfer (8 bits spent
//! where the eye sees banding), and an sRGB format leaves only ONE linear channel (alpha) for two
//! scalars.
//!
//! ## One texel per vertex
//!
//! A bake is N × N texels for a tile of N × N samples, texel (i, j) AT sample (i, j) — not N − 1
//! texels centred on the cells. Three things follow, and the renderer relies on each:
//!
//! * sampling at a vertex returns that vertex's texel, untouched by any filter weight;
//! * a child vertex (i, j) sits at parent sample ((offset + i) / 2, (offset + j) / 2): on a parent
//!   texel when even, exactly midway between two when odd — the same dyadic positions the geometry
//!   morph uses, so appearance and geometry are interpolated on the same lattice;
//! * the texels along an edge two tiles share describe the SAME world samples, so they can be
//!   made bit-identical (below), and the two tiles then shade the shared edge identically.
//!
//! No mip chain: the tile is drawn with one texel per vertex at every distance it is selected for
//! (a farther camera selects a coarser tile, which is the mip).
//!
//! ## The level-0 appearance of a sample
//!
//! The m19.7c (+ fix 1) blend of `terrain.frag`, evaluated at the sample's position with each
//! layer reduced to constants: its MEAN albedo (the cooked albedo texture's level 0, decoded to
//! linear light and averaged) times the material's base colour, the material's metallic and
//! roughness, and its mean height times its contrast as the effective height. The painted weights
//! are the splat map sampled bilinearly at the sample, corner-aligned as the shader samples it.
//!
//! **Detail below the parent's texel spacing is averaged away by design.** A layer's texture
//! pattern, and the way its height map breaks up a transition, do not survive: the bake knows a
//! layer by its mean. That is the right content for a tile drawn too far away to resolve them.
//!
//! ## The footprint, and why it is defined in WORLD sample space
//!
//! A parent texel at level L averages the level-0 samples within half a parent cell of it: global
//! sample offsets −2^(L−1) ..= +2^(L−1) on each axis, the two end samples at half weight (a box
//! one parent cell wide, centred on the texel). The footprint is a function of the texel's GLOBAL
//! sample position only: it reads whichever level-0 tiles hold those samples, across its own
//! tile's border. A texel on an edge two parents share therefore has the same footprint from both
//! sides, and the two bakes carry the same bytes there. (A footprint clipped to the parent's own
//! tile would be one-sided on each side of the edge — a visible seam between every pair of coarse
//! tiles.)
//!
//! Two details make "the same from both sides" literal:
//!
//! * a sample on a level-0 tile border is held by two (or four) tiles whose splat maps need not
//!   agree there; its value is the MEAN over the tiles that hold it;
//! * a sample no tile holds (beyond the world's border, or over a tile with no palette) is
//!   dropped and the weights renormalised — the world-border "clamp", the same from either side.
//!
//! ## Linear light
//!
//! Every average here — the layer's mean albedo and the footprint — is taken in LINEAR light and
//! encoded to sRGB once, at the end. sRGB bytes are not proportional to light: averaging them
//! darkens any mix of light and dark (a black/white checker averages to 50 % of the code range,
//! which is 21 % of the light). The level-0 pixels a parent replaces are filtered in linear light
//! by the GPU, so a bake averaged in sRGB would be visibly darker than the ground it stands for.
//!
//! ## Which parents get a bake
//!
//! One whose every level-0 tile beneath it has a splat palette. A parent over a palette-less
//! (v1) tile gets none and is drawn with the same flat material its children are — the engine
//! counts those draws. A palette id the layer table cannot resolve refuses the cook.

use std::collections::BTreeMap;
use std::path::Path;

use crate::cooked::{
    fnv1a_64, read_header, ASSET_KIND_MATERIAL, ASSET_KIND_TERRAIN_LAYER, ASSET_KIND_TEXTURE,
};
use crate::heightfield::{Heightfield, SPLAT_LAYER_COUNT};
use crate::terrain_world::{LodWorld, Refusal, RefusalKind, WorldCookError};
use crate::texture::TEXFMT_RGBA8_SRGB;
use crate::PipelineError;

/// A palette layer reduced to the constants the bake blends.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LayerAppearance {
    /// Linear RGB: the material's base colour times the layer texture's mean albedo.
    pub color: [f64; 3],
    pub metallic: f64,
    pub roughness: f64,
    /// `height_contrast × mean height` — the m19.7c fix-1 effective height, in halvings.
    pub effective_height: f64,
}

/// Palette AssetId → its constants.
pub type LayerTable = BTreeMap<u64, LayerAppearance>;

/// One sample's blended appearance: linear r, g, b, then metallic, roughness.
pub type Appearance = [f64; 5];

/// One parent tile's bake: two N × N RGBA8 grids, row-major, x fastest (texel (i, j) at sample
/// (i, j)).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TileBake {
    /// sRGB-encoded base colour, A = 255.
    pub color: Vec<u8>,
    /// R = metallic, G = roughness (linear, UNORM), B = 0, A = 255.
    pub material: Vec<u8>,
}

/// The bakes of a world, indexed like `LodWorld::tiles` (level 0 is always `None`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WorldBake {
    pub tiles: Vec<Option<TileBake>>,
    /// Parents left without a bake because a level-0 tile beneath them has no palette.
    pub parents_unbaked: usize,
}

/// The sRGB EOTF (IEC 61966-2-1) of one byte, in f64.
pub fn srgb_to_linear(byte: u8) -> f64 {
    let c = f64::from(byte) / 255.0;
    if c <= 0.04045 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}

/// Linear light → the nearest sRGB byte.
pub fn linear_to_srgb(l: f64) -> u8 {
    let s = if l <= 0.003_130_8 {
        12.92 * l
    } else {
        1.055 * l.powf(1.0 / 2.4) - 0.055
    };
    (s * 255.0).round().clamp(0.0, 255.0) as u8
}

fn unorm8(v: f64) -> u8 {
    (v * 255.0).round().clamp(0.0, 255.0) as u8
}

/// `terrain.frag`'s `height_blend`, on the CPU: the painted weights `w` redistributed by the
/// layers' effective heights `e`. Bypassed (returns `w`) when every painted layer has the same
/// `e`, exactly as the shader is.
pub fn height_blend(
    w: [f64; SPLAT_LAYER_COUNT],
    e: [f64; SPLAT_LAYER_COUNT],
) -> [f64; SPLAT_LAYER_COUNT] {
    let mut e_ref = -1.0f64;
    let mut e_min = f64::INFINITY;
    for k in 0..SPLAT_LAYER_COUNT {
        if w[k] > 0.0 {
            e_ref = e_ref.max(e[k]);
            e_min = e_min.min(e[k]);
        }
    }
    if e_ref < 0.0 || e_ref == e_min {
        return w;
    }
    let mut q = [0.0; SPLAT_LAYER_COUNT];
    for k in 0..SPLAT_LAYER_COUNT {
        q[k] = w[k] * (e[k] - e_ref).min(0.0).exp2();
    }
    let sum: f64 = q.iter().sum();
    q.map(|v| v / sum)
}

/// Where sample `i` of `n` lands on a corner-aligned axis of `texels` weight texels: the lower
/// texel, the upper one (clamped) and the fraction between them. Integer division, so the two
/// ends land exactly on the first and last texel — `terrain.frag`'s `(local / extent) · (dims − 1)`.
fn weight_axis(i: u32, n: u32, texels: u32) -> (usize, usize, f64) {
    let (num, den) = (u64::from(i) * u64::from(texels - 1), u64::from(n - 1));
    let lo = (num / den) as usize;
    let hi = (lo + 1).min(texels as usize - 1);
    (lo, hi, (num % den) as f64 / den as f64)
}

/// The level-0 appearance of every sample of one tile (row-major, x fastest), or `None` for a
/// tile without a palette. An unresolvable palette id is pushed onto `refusals`.
pub fn tile_appearance(
    x: i32,
    z: i32,
    hf: &Heightfield,
    layers: &LayerTable,
    refusals: &mut Vec<Refusal>,
) -> Option<Vec<Appearance>> {
    let splat = hf.splat.as_ref()?;
    // An unused slot repeats layer 0, as the pass's uniform block does; its weight is 0.
    let mut slots = [None; SPLAT_LAYER_COUNT];
    for (k, slot) in slots.iter_mut().enumerate() {
        let id = hf.sidecar.layers[k].or(hf.sidecar.layers[0])?;
        *slot = layers.get(&id).copied();
        if slot.is_none() {
            refusals.push(Refusal {
                kind: RefusalKind::UnresolvedLayer,
                detail: format!(
                    "tile ({x}, {z}) layer{k} = 0x{id:016x} is not a cooked Material or \
                     TerrainLayer (with an RGBA8 sRGB texture) in the palette directory"
                ),
            });
        }
    }
    let slots: Vec<LayerAppearance> = slots.into_iter().collect::<Option<_>>()?;
    let e: [f64; SPLAT_LAYER_COUNT] = std::array::from_fn(|k| slots[k].effective_height);
    let channels = |l: &LayerAppearance| -> Appearance {
        [l.color[0], l.color[1], l.color[2], l.metallic, l.roughness]
    };
    let p: Vec<Appearance> = slots.iter().map(channels).collect();
    let texel = |tx: usize, tz: usize, k: usize| {
        f64::from(splat.weights[4 * (tx + splat.columns as usize * tz) + k]) / 255.0
    };
    let mut out = Vec::with_capacity((hf.columns * hf.rows) as usize);
    for j in 0..hf.rows {
        let (z0, z1, fz) = weight_axis(j, hf.rows, splat.rows);
        for i in 0..hf.columns {
            let (x0, x1, fx) = weight_axis(i, hf.columns, splat.columns);
            let w: [f64; SPLAT_LAYER_COUNT] = std::array::from_fn(|k| {
                (1.0 - fz) * ((1.0 - fx) * texel(x0, z0, k) + fx * texel(x1, z0, k))
                    + fz * ((1.0 - fx) * texel(x0, z1, k) + fx * texel(x1, z1, k))
            });
            let b = height_blend(w, e);
            // The shader's difference form: layer 0's share is implied.
            let a: Appearance = std::array::from_fn(|c| {
                p[0][c]
                    + b[1] * (p[1][c] - p[0][c])
                    + b[2] * (p[2][c] - p[0][c])
                    + b[3] * (p[3][c] - p[0][c])
            });
            out.push(a);
        }
    }
    Some(out)
}

/// Bake every parent of `world` whose level-0 tiles all carry a palette.
pub fn bake_world(world: &LodWorld, layers: &LayerTable) -> Result<WorldBake, WorldCookError> {
    let n = world.samples;
    let cells = i64::from(n - 1);
    let mut refusals = Vec::new();
    // Level-0 appearance per tile, keyed by coordinate.
    let mut level0: BTreeMap<(i32, i32), Option<Vec<Appearance>>> = BTreeMap::new();
    for t in world.tiles.iter().filter(|t| t.level == 0) {
        level0.insert(
            (t.x, t.z),
            tile_appearance(t.x, t.z, &t.heightfield, layers, &mut refusals),
        );
    }
    if !refusals.is_empty() {
        return Err(WorldCookError { refusals });
    }

    // The value of a GLOBAL level-0 sample: the mean over the tiles that hold it (one inside a
    // tile, two on a border, four on a corner) and have an appearance. `None` = nobody does.
    let sample = |gx: i64, gz: i64| -> Option<Appearance> {
        let owners = |g: i64| {
            let (t, i) = (g.div_euclid(cells), g.rem_euclid(cells));
            // A border sample is also the LAST sample of the tile before.
            [Some((t, i)), (i == 0).then_some((t - 1, cells))]
        };
        let mut sum = [0.0f64; 5];
        let mut count = 0u32;
        for (tz, j) in owners(gz).into_iter().flatten() {
            for (tx, i) in owners(gx).into_iter().flatten() {
                let (Ok(tx), Ok(tz)) = (i32::try_from(tx), i32::try_from(tz)) else {
                    continue;
                };
                if let Some(Some(app)) = level0.get(&(tx, tz)) {
                    let a = &app[(i + i64::from(n) * j) as usize];
                    for c in 0..5 {
                        sum[c] += a[c];
                    }
                    count += 1;
                }
            }
        }
        (count > 0).then(|| sum.map(|v| v / f64::from(count)))
    };

    let mut tiles = Vec::with_capacity(world.tiles.len());
    let mut parents_unbaked = 0;
    for t in &world.tiles {
        if t.level == 0 {
            tiles.push(None);
            continue;
        }
        let side = 1i32 << t.level;
        let complete = (0..side).all(|dz| {
            (0..side).all(|dx| {
                matches!(
                    level0.get(&(t.x * side + dx, t.z * side + dz)),
                    Some(Some(_))
                )
            })
        });
        if !complete {
            parents_unbaked += 1;
            tiles.push(None);
            continue;
        }
        let half = 1i64 << (t.level - 1);
        let (bx, bz) = (i64::from(t.x) * cells, i64::from(t.z) * cells);
        let mut color = Vec::with_capacity((n * n * 4) as usize);
        let mut material = Vec::with_capacity((n * n * 4) as usize);
        for j in 0..i64::from(n) {
            for i in 0..i64::from(n) {
                // The texel's global level-0 sample, and the box one parent cell wide around it.
                let (gx, gz) = ((bx + i) << t.level, (bz + j) << t.level);
                let mut sum = [0.0f64; 5];
                let mut total = 0.0f64;
                for dz in -half..=half {
                    let wz = if dz.abs() == half { 0.5 } else { 1.0 };
                    for dx in -half..=half {
                        let w = wz * if dx.abs() == half { 0.5 } else { 1.0 };
                        if let Some(a) = sample(gx + dx, gz + dz) {
                            for c in 0..5 {
                                sum[c] += w * a[c];
                            }
                            total += w;
                        }
                    }
                }
                // Never empty: the centre sample is one of this parent's own.
                let a = sum.map(|v| v / total);
                // Averaged in linear light; encoded ONCE, here.
                color.extend_from_slice(&[
                    linear_to_srgb(a[0]),
                    linear_to_srgb(a[1]),
                    linear_to_srgb(a[2]),
                    255,
                ]);
                material.extend_from_slice(&[unorm8(a[3]), unorm8(a[4]), 0, 255]);
            }
        }
        tiles.push(Some(TileBake { color, material }));
    }
    Ok(WorldBake {
        tiles,
        parents_unbaked,
    })
}

fn le_u32(b: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(at..at + 4)?.try_into().ok()?))
}

fn le_u64(b: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(at..at + 8)?.try_into().ok()?))
}

fn le_f64(b: &[u8], at: usize) -> Option<f64> {
    Some(f64::from(f32::from_le_bytes(
        b.get(at..at + 4)?.try_into().ok()?,
    )))
}

/// The mean of a cooked RGBA8 sRGB texture's level 0: linear RGB, and alpha / 255. `None` for any
/// other format (a block-compressed layer texture cannot be averaged without decoding it).
fn texture_mean(payload: &[u8]) -> Option<([f64; 3], f64)> {
    let (w, h, format, mips) = (
        le_u32(payload, 0)?,
        le_u32(payload, 4)?,
        le_u32(payload, 8)?,
        le_u32(payload, 12)?,
    );
    if format != TEXFMT_RGBA8_SRGB || mips == 0 {
        return None;
    }
    let blob = 16 + 16 * mips as usize;
    let (offset, size) = (le_u32(payload, 24)? as usize, le_u32(payload, 28)? as usize);
    let texels = w as usize * h as usize;
    if texels == 0 || size != texels * 4 {
        return None;
    }
    let level0 = payload.get(blob + offset..blob + offset + size)?;
    let mut sum = [0.0f64; 4];
    for t in level0.as_chunks::<4>().0 {
        for c in 0..3 {
            sum[c] += srgb_to_linear(t[c]);
        }
        sum[3] += f64::from(t[3]) / 255.0;
    }
    let m = sum.map(|v| v / texels as f64);
    Some(([m[0], m[1], m[2]], m[3]))
}

/// Build a layer table from cooked RMA1 payloads keyed by AssetId: `(asset kind, payload)`.
/// A `Material` resolves to its base colour, metallic and roughness (height 0); a `TerrainLayer`
/// to its material's scalars with the colour tinted by its texture's mean albedo, and its
/// contrast times the texture's mean height. A layer whose material or texture is missing or of
/// the wrong shape is left OUT of the table, so a tile that names it is refused by `bake_world`.
/// This mirrors the engine's `TerrainLayerBuilder` dispatch (ADR-0066 §2).
pub fn layer_table(assets: &BTreeMap<u64, (u16, Vec<u8>)>) -> LayerTable {
    let material = |p: &[u8]| -> Option<LayerAppearance> {
        // MaterialV1: base_color[4], emissive[3], metallic, roughness, … (material.rs).
        Some(LayerAppearance {
            color: [le_f64(p, 0)?, le_f64(p, 4)?, le_f64(p, 8)?],
            metallic: le_f64(p, 28)?,
            roughness: le_f64(p, 32)?,
            effective_height: 0.0,
        })
    };
    let of_kind = |id: u64, kind: u16| assets.get(&id).filter(|a| a.0 == kind).map(|a| &a.1[..]);
    let mut table = LayerTable::new();
    for (&id, (kind, payload)) in assets {
        let entry = match *kind {
            ASSET_KIND_MATERIAL => material(payload),
            ASSET_KIND_TERRAIN_LAYER => (|| {
                // TerrainLayerV1: material, albedo_height, uv_scale[2], height_contrast.
                let mut l = material(of_kind(le_u64(payload, 0)?, ASSET_KIND_MATERIAL)?)?;
                let (albedo, height) =
                    texture_mean(of_kind(le_u64(payload, 8)?, ASSET_KIND_TEXTURE)?)?;
                for (c, a) in l.color.iter_mut().zip(albedo) {
                    *c *= a;
                }
                l.effective_height = le_f64(payload, 24)? * height;
                Some(l)
            })(),
            _ => None,
        };
        if let Some(e) = entry {
            table.insert(id, e);
        }
    }
    table
}

/// Read every cooked RMA1 file directly in `dir` and build the layer table from them. Files that
/// are not RMA1 are skipped; the result does not depend on directory order (ids are content
/// hashes).
pub fn load_layer_table(dir: &Path) -> Result<LayerTable, PipelineError> {
    let mut assets = BTreeMap::new();
    for entry in std::fs::read_dir(dir)? {
        let path = entry?.path();
        if !path.is_file() {
            continue;
        }
        let bytes = std::fs::read(&path)?;
        if let Ok((header, payload)) = read_header(&bytes) {
            assets.insert(fnv1a_64(payload), (header.asset_kind, payload.to_vec()));
        }
    }
    Ok(layer_table(&assets))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::heightfield::HeightfieldSidecar;
    use crate::material::Material;
    use crate::terrain_layer::TerrainLayer;
    use crate::terrain_world::{build_lod_world, cook_lod_world, cook_lod_world_baked};
    use crate::texture::{ColorSpace, Texture};

    const N: u32 = 9;
    const SIZE: f32 = 8.0;

    /// A small deterministic generator (splitmix64), so the "random" world is the same every run.
    struct Rng(u64);

    impl Rng {
        fn next(&mut self) -> u64 {
            self.0 = self.0.wrapping_add(0x9e37_79b9_7f4a_7c15);
            let mut z = self.0;
            z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
            z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
            z ^ (z >> 31)
        }

        fn unit(&mut self) -> f64 {
            (self.next() >> 11) as f64 / (1u64 << 53) as f64
        }
    }

    const IDS: [u64; 6] = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66];

    fn random_layers(rng: &mut Rng) -> LayerTable {
        IDS.iter()
            .map(|&id| {
                (
                    id,
                    LayerAppearance {
                        color: [rng.unit(), rng.unit(), rng.unit()],
                        metallic: rng.unit(),
                        roughness: rng.unit(),
                        // Two layers share an effective height of 0, so the bypass is exercised.
                        effective_height: if id <= 0x22 { 0.0 } else { 6.0 * rng.unit() },
                    },
                )
            })
            .collect()
    }

    /// A flat tile at (x, z) with `layers` and a `wc × wr` splat map from `texel`.
    fn tile(
        x: i32,
        z: i32,
        layers: [Option<u64>; 4],
        (wc, wr): (u32, u32),
        mut texel: impl FnMut() -> [u8; 4],
    ) -> (i32, i32, Heightfield) {
        let sidecar = HeightfieldSidecar {
            size_x: SIZE,
            size_z: SIZE,
            height_min: 0.0,
            height_max: 65.535,
            origin: [x as f32 * SIZE, 0.0, z as f32 * SIZE],
            columns: None,
            rows: None,
            layers,
        };
        let hf = Heightfield::new(N, N, vec![1000; (N * N) as usize], sidecar).unwrap();
        if layers[0].is_none() {
            return (x, z, hf);
        }
        let mut rgba = Vec::new();
        for _ in 0..wc * wr {
            let mut t = texel();
            for k in 0..4 {
                if layers[k].is_none() {
                    t[k] = 0;
                }
            }
            if t.iter().all(|&v| v == 0) {
                t[0] = 1;
            }
            rgba.extend_from_slice(&t);
        }
        (x, z, hf.with_splat(wc, wr, &rgba).unwrap())
    }

    /// A w × h world of tiles with random palettes (2–4 of the six layers), random splat maps of
    /// random sizes — nothing agrees across a tile border except by accident.
    fn random_world(rng: &mut Rng, w: i32, h: i32, levels: u32) -> LodWorld {
        let mut tiles = Vec::new();
        for z in 0..h {
            for x in 0..w {
                let used = 2 + (rng.next() % 3) as usize;
                let first = (rng.next() % 6) as usize;
                let layers: [Option<u64>; 4] =
                    std::array::from_fn(|k| (k < used).then(|| IDS[(first + k) % 6]));
                let dims = (1 + (rng.next() % 6) as u32, 1 + (rng.next() % 6) as u32);
                tiles.push(tile(x, z, layers, dims, || {
                    let r = rng.next();
                    // A quarter of the channels are exactly 0: unpainted layers must be common.
                    std::array::from_fn(|k| {
                        let v = (r >> (16 * k)) as u16;
                        if v.is_multiple_of(4) {
                            0
                        } else {
                            (v >> 8) as u8
                        }
                    })
                }));
            }
        }
        build_lod_world(tiles, levels).unwrap()
    }

    /// BRUTE FORCE, organised differently from the cook on purpose: a dense map of every global
    /// sample to the values of ALL tiles holding it, filled tile by tile; the appearance from the
    /// naive weighted sum Σ b·p (the cook uses the difference form) with a float texel coordinate
    /// (the cook divides integers); the footprint as an explicit weight kernel.
    fn brute(world: &LodWorld, layers: &LayerTable) -> Vec<Option<TileBake>> {
        let n = world.samples as i64;
        let cells = n - 1;
        let mut held: BTreeMap<(i64, i64), Vec<Appearance>> = BTreeMap::new();
        let mut has_palette: BTreeMap<(i32, i32), bool> = BTreeMap::new();
        for t in world.tiles.iter().filter(|t| t.level == 0) {
            let hf = &t.heightfield;
            has_palette.insert((t.x, t.z), hf.splat.is_some());
            let Some(splat) = &hf.splat else { continue };
            let slot: Vec<LayerAppearance> = (0..4)
                .map(|k| layers[&hf.sidecar.layers[k].or(hf.sidecar.layers[0]).unwrap()])
                .collect();
            let (wc, wr) = (splat.columns as i64, splat.rows as i64);
            for j in 0..n {
                for i in 0..n {
                    let u = (i * (wc - 1)) as f64 / cells as f64;
                    let v = (j * (wr - 1)) as f64 / cells as f64;
                    let (x0, z0) = (u.floor() as i64, v.floor() as i64);
                    let (fx, fz) = (u - u.floor(), v - v.floor());
                    let mut w = [0.0f64; 4];
                    for (dz, kz) in [(0, 1.0 - fz), (1, fz)] {
                        for (dx, kx) in [(0, 1.0 - fx), (1, fx)] {
                            let (tx, tz) = ((x0 + dx).min(wc - 1), (z0 + dz).min(wr - 1));
                            for (k, wk) in w.iter_mut().enumerate() {
                                let byte = splat.weights[(4 * (tx + wc * tz)) as usize + k];
                                *wk += kx * kz * f64::from(byte) / 255.0;
                            }
                        }
                    }
                    // m19.7c fix 1, written from the ADR rather than from the cook.
                    let painted: Vec<usize> = (0..4).filter(|&k| w[k] > 0.0).collect();
                    let e = |k: usize| slot[k].effective_height;
                    let top = painted.iter().map(|&k| e(k)).fold(f64::MIN, f64::max);
                    let b = if painted.iter().all(|&k| e(k) == top) {
                        w
                    } else {
                        let g: Vec<f64> = (0..4)
                            .map(|k| w[k] * 2f64.powf((e(k) - top).min(0.0)))
                            .collect();
                        let s: f64 = g.iter().sum();
                        std::array::from_fn(|k| g[k] / s)
                    };
                    // Naive Σ b·p needs Σ b = 1; the bypass returns w, whose bilinear sum is 1
                    // only to rounding — so restate layer 0's share as the remainder, which is
                    // what the shader's difference form means.
                    let b0 = 1.0 - b[1] - b[2] - b[3];
                    let ch = |l: &LayerAppearance, c: usize| match c {
                        0..=2 => l.color[c],
                        3 => l.metallic,
                        _ => l.roughness,
                    };
                    let a: Appearance = std::array::from_fn(|c| {
                        b0 * ch(&slot[0], c)
                            + b[1] * ch(&slot[1], c)
                            + b[2] * ch(&slot[2], c)
                            + b[3] * ch(&slot[3], c)
                    });
                    held.entry((i64::from(t.x) * cells + i, i64::from(t.z) * cells + j))
                        .or_default()
                        .push(a);
                }
            }
        }
        world
            .tiles
            .iter()
            .map(|t| {
                if t.level == 0 {
                    return None;
                }
                let side = 1i32 << t.level;
                for dz in 0..side {
                    for dx in 0..side {
                        if !has_palette[&(t.x * side + dx, t.z * side + dz)] {
                            return None;
                        }
                    }
                }
                let step = 1i64 << t.level;
                let kernel: Vec<(i64, f64)> = (-step / 2..=step / 2)
                    .map(|d| (d, if d.abs() == step / 2 { 0.5 } else { 1.0 }))
                    .collect();
                let (mut color, mut material) = (Vec::new(), Vec::new());
                for j in 0..n {
                    for i in 0..n {
                        let g = (
                            (i64::from(t.x) * cells + i) * step,
                            (i64::from(t.z) * cells + j) * step,
                        );
                        let (mut sum, mut total) = ([0.0f64; 5], 0.0f64);
                        for &(dz, wz) in &kernel {
                            for &(dx, wx) in &kernel {
                                let Some(vals) = held.get(&(g.0 + dx, g.1 + dz)) else {
                                    continue;
                                };
                                for c in 0..5 {
                                    let mean =
                                        vals.iter().map(|a| a[c]).sum::<f64>() / vals.len() as f64;
                                    sum[c] += wx * wz * mean;
                                }
                                total += wx * wz;
                            }
                        }
                        let enc = |l: f64| {
                            let s = if l <= 0.0031308 {
                                l * 12.92
                            } else {
                                1.055 * l.powf(1.0 / 2.4) - 0.055
                            };
                            (s * 255.0).round() as u8
                        };
                        color.extend_from_slice(&[
                            enc(sum[0] / total),
                            enc(sum[1] / total),
                            enc(sum[2] / total),
                            255,
                        ]);
                        material.extend_from_slice(&[
                            (sum[3] / total * 255.0).round() as u8,
                            (sum[4] / total * 255.0).round() as u8,
                            0,
                            255,
                        ]);
                    }
                }
                Some(TileBake { color, material })
            })
            .collect()
    }

    #[test]
    fn the_bake_equals_a_brute_force_on_a_randomised_world() {
        // Proof (a), Rust half. Three seeds, two world shapes; every byte of every bake.
        for (seed, w, h, levels) in [(1u64, 4, 4, 3), (2, 4, 2, 2), (3, 4, 4, 3)] {
            let mut rng = Rng(seed);
            let layers = random_layers(&mut rng);
            let world = random_world(&mut rng, w, h, levels);
            let bake = bake_world(&world, &layers).unwrap();
            let want = brute(&world, &layers);
            assert_eq!(bake.parents_unbaked, 0);
            let parents = world.tiles.iter().filter(|t| t.level > 0).count();
            assert_eq!(bake.tiles.iter().flatten().count(), parents);
            assert!(parents >= 2);
            for (k, (got, want)) in bake.tiles.iter().zip(&want).enumerate() {
                assert_eq!(
                    got, want,
                    "seed {seed}: tile {k} differs from the brute force"
                );
            }
            // Not vacuous: the bakes are not one flat colour.
            let b = bake.tiles.iter().flatten().next().unwrap();
            assert!(b.color.chunks(4).any(|t| t[..3] != b.color[..3]));
            assert!(b.material.chunks(4).any(|t| t[..2] != b.material[..2]));
        }
    }

    #[test]
    fn shared_parent_edges_carry_identical_texels() {
        // Proof (c), Rust half: every pair of same-level parents that share an edge agrees on it,
        // byte for byte, although no two level-0 tiles agree on anything along their borders.
        let mut rng = Rng(7);
        let layers = random_layers(&mut rng);
        let world = random_world(&mut rng, 8, 4, 3);
        let bake = bake_world(&world, &layers).unwrap();
        let n = N as usize;
        let at = |b: &TileBake, i: usize, j: usize| {
            let o = 4 * (i + n * j);
            (b.color[o..o + 4].to_vec(), b.material[o..o + 4].to_vec())
        };
        let find = |level, x, z| {
            let k = world
                .tiles
                .iter()
                .position(|t| t.level == level && t.x == x && t.z == z)?;
            bake.tiles[k].as_ref()
        };
        let (mut pairs, mut texels, mut one_sided_would_differ) = (0, 0, 0);
        for t in world.tiles.iter().filter(|t| t.level > 0) {
            let me = find(t.level, t.x, t.z).unwrap();
            if let Some(east) = find(t.level, t.x + 1, t.z) {
                pairs += 1;
                for j in 0..n {
                    assert_eq!(at(me, n - 1, j), at(east, 0, j));
                    texels += 1;
                    // The witness: one texel INSIDE each tile differs, so equal edges are not
                    // just a flat world.
                    one_sided_would_differ += usize::from(at(me, n - 2, j) != at(east, 1, j));
                }
            }
            if let Some(north) = find(t.level, t.x, t.z + 1) {
                pairs += 1;
                for i in 0..n {
                    assert_eq!(at(me, i, n - 1), at(north, i, 0));
                    texels += 1;
                }
            }
        }
        assert_eq!(pairs, 10 + 1); // 8 level-1 parents in a 4×2 grid (10 edges), 2 roots (1)
        assert_eq!(texels, 11 * n);
        assert!(one_sided_would_differ > 4 * n);
    }

    #[test]
    fn averages_are_taken_in_linear_light() {
        // A black layer and a white layer painted half and half: 50 % of the LIGHT, which is sRGB
        // byte 188 — not 128, the average of the bytes.
        let layers: LayerTable = [(0x11, [0.0; 3]), (0x22, [1.0; 3])]
            .into_iter()
            .map(|(id, color)| {
                (
                    id,
                    LayerAppearance {
                        color,
                        metallic: 0.25,
                        roughness: 0.5,
                        effective_height: 0.0,
                    },
                )
            })
            .collect();
        let tiles = (0..4)
            .map(|k| {
                tile(
                    k % 2,
                    k / 2,
                    [Some(0x11), Some(0x22), None, None],
                    (1, 1),
                    || {
                        [51, 204, 0, 0] // already sums to 255: 20 % black, 80 % white
                    },
                )
            })
            .collect();
        let world = build_lod_world(tiles, 2).unwrap();
        let bake = bake_world(&world, &layers).unwrap();
        let b = bake.tiles.last().unwrap().as_ref().unwrap();
        assert_eq!(linear_to_srgb(0.8), 231);
        for t in b.color.chunks(4) {
            assert_eq!(t, [231, 231, 231, 255]); // 0.8 of the light; 204 would be the byte mean
        }
        for t in b.material.chunks(4) {
            assert_eq!(t, [64, 128, 0, 255]);
        }
        // And the layer's own mean: a black/white checker texture is 50 % light = byte 188.
        let checker: Vec<u8> = (0..16)
            .flat_map(|k| {
                let v = if (k + k / 4) % 2 == 0 { 0 } else { 255 };
                [v, v, v, 51 * (k % 2) as u8]
            })
            .collect();
        let tex = Texture::from_rgba8(4, 4, ColorSpace::Srgb, checker).cook();
        let (_, payload) = read_header(&tex.0).unwrap();
        let (albedo, height) = texture_mean(payload).unwrap();
        assert_eq!(albedo, [0.5; 3]);
        assert_eq!(linear_to_srgb(albedo[0]), 188);
        assert!((height - 0.1).abs() < 1e-12);
    }

    #[test]
    fn every_byte_round_trips_through_the_transfer_functions() {
        for b in 0..=255u8 {
            assert_eq!(linear_to_srgb(srgb_to_linear(b)), b);
        }
    }

    #[test]
    fn a_parent_over_a_tile_without_a_palette_gets_no_bake_and_an_unknown_layer_is_refused() {
        let mut rng = Rng(11);
        let layers = random_layers(&mut rng);
        // 4×2 tiles, levels 2: two parents. Tile (3, 1) has no palette, so the east parent is
        // unbaked; the west one still is, its east edge averaging only what exists.
        let mut tiles = Vec::new();
        for z in 0..2 {
            for x in 0..4 {
                let layers = if (x, z) == (3, 1) {
                    [None; 4]
                } else {
                    [Some(0x33), Some(0x44), None, None]
                };
                tiles.push(tile(x, z, layers, (3, 3), || {
                    [(rng.next() % 256) as u8, (rng.next() % 256) as u8, 0, 0]
                }));
            }
        }
        let world = build_lod_world(tiles.clone(), 2).unwrap();
        let bake = bake_world(&world, &layers).unwrap();
        assert_eq!(bake.parents_unbaked, 1);
        assert_eq!(bake.tiles.iter().flatten().count(), 1);
        assert_eq!(bake.tiles, brute(&world, &layers));

        // The manifest: level-0 lines and the unbaked parent keep 10 fields, the baked one has 14,
        // and a cook without a table is m19.8d1's, byte for byte.
        let cooked = cook_lod_world_baked("w", world.clone(), Some(&layers)).unwrap();
        let fields: Vec<usize> = cooked
            .manifest
            .lines()
            .filter(|l| l.starts_with("tile\t"))
            .map(|l| l.split('\t').count())
            .collect();
        assert_eq!(fields, [10, 10, 10, 10, 10, 10, 10, 10, 14, 10]);
        assert_eq!(cooked.files.len(), 10 + 2);
        assert!(cooked.manifest.contains("\tw_L1_0_0_bake_color.rtex\t"));
        assert!(cooked.manifest.contains("\tw_L1_0_0_bake_material.rtex\n"));
        let plain = cook_lod_world("w", world.clone());
        assert!(plain.bake.is_none());
        assert_eq!(plain.files, cooked.files[..10]);
        assert_eq!(
            plain.manifest,
            world.manifest_text(&plain_names(&plain.files))
        );

        // The cooked textures: single level, N×N, the right formats, the bake's bytes.
        for (file, format, want) in [
            (
                &cooked.files[10],
                TEXFMT_RGBA8_SRGB,
                &bake.tiles[8].as_ref().unwrap().color,
            ),
            (
                &cooked.files[11],
                0,
                &bake.tiles[8].as_ref().unwrap().material,
            ),
        ] {
            let (header, p) = read_header(&file.1).unwrap();
            assert_eq!(header.asset_kind, ASSET_KIND_TEXTURE);
            let word = |k: usize| le_u32(p, 4 * k).unwrap();
            assert_eq!([word(0), word(1), word(2), word(3)], [N, N, format, 1]);
            assert_eq!([word(4), word(5), word(6), word(7)], [N, N, 0, N * N * 4]);
            assert_eq!(&p[32..], &want[..]);
        }

        // An id the table does not hold: refused, once per slot that names it.
        tiles[0].2.sidecar.layers[1] = Some(0xdead);
        let world = build_lod_world(tiles, 2).unwrap();
        let err = bake_world(&world, &layers).unwrap_err();
        assert_eq!(err.count(RefusalKind::UnresolvedLayer), 1);
        assert!(err.to_string().contains("0x000000000000dead"));
    }

    #[test]
    fn the_description_cook_reads_the_palette_directory_and_bakes() {
        // The on-disk path end to end: sources + `palette_dir` of COOKED assets -> the same bake
        // the in-memory cook produces from the same world and the table those assets resolve to.
        use crate::terrain_world::cook_terrain_world;
        let dir = std::env::temp_dir().join(format!("rime-bake-cook-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("cooked")).unwrap();
        let grass = Material {
            base_color: [0.2, 0.6, 0.1, 1.0],
            roughness: 0.9,
            ..Material::default()
        }
        .cook();
        let rock = Material {
            base_color: [0.4, 0.4, 0.45, 1.0],
            metallic: 0.2,
            roughness: 0.6,
            ..Material::default()
        }
        .cook();
        let texels: Vec<u8> = (0..16u8)
            .flat_map(|k| [40 + 10 * k, 200 - 5 * k, 90, 16 * k])
            .collect();
        let texture = Texture::from_rgba8(4, 4, ColorSpace::Srgb, texels).cook();
        let gravel = TerrainLayer {
            material: rock.1,
            albedo_height: texture.1,
            uv_scale: [2.0, 2.0],
            height_contrast: 5.0,
        }
        .cook();
        for (name, asset) in [
            ("grass.rmat", &grass),
            ("rock.rmat", &rock),
            ("gravel.rtex", &texture),
            ("gravel.rtl", &gravel),
        ] {
            std::fs::write(dir.join("cooked").join(name), &asset.0).unwrap();
        }
        std::fs::write(dir.join("cooked/notes.txt"), b"not an RMA1 file").unwrap();

        let mut rng = Rng(21);
        let mut desc = String::from("levels = 2\npalette_dir = \"cooked\"\n");
        let mut tiles = Vec::new();
        for (x, z) in [(0, 0), (1, 0), (0, 1), (1, 1)] {
            let stem = format!("t_{x}_{z}");
            let samples = vec![1000u16; (N * N) as usize];
            let raw: Vec<u8> = samples.iter().flat_map(|q| q.to_le_bytes()).collect();
            std::fs::write(dir.join(format!("{stem}.r16")), raw).unwrap();
            std::fs::write(
                dir.join(format!("{stem}.toml")),
                format!(
                    "size_x = {SIZE}\nsize_z = {SIZE}\nheight_min = 0\nheight_max = 65.535\n\
                     origin_x = {}\norigin_z = {}\ncolumns = {N}\nrows = {N}\n\
                     layer0 = 0x{:x}\nlayer1 = 0x{:x}\n",
                    x as f32 * SIZE,
                    z as f32 * SIZE,
                    grass.1,
                    gravel.1
                ),
            )
            .unwrap();
            let rgba: Vec<u8> = (0..3 * 2)
                .flat_map(|_| [1 + (rng.next() % 255) as u8, (rng.next() % 256) as u8, 0, 0])
                .collect();
            image::RgbaImage::from_raw(3, 2, rgba.clone())
                .unwrap()
                .save(dir.join(format!("{stem}.splat.png")))
                .unwrap();
            desc.push_str(&format!("tile_{x}_{z} = \"{stem}.r16\"\n"));
            tiles.push(tile(
                x,
                z,
                [Some(grass.1), Some(gravel.1), None, None],
                (3, 2),
                {
                    let mut it = rgba
                        .chunks(4)
                        .map(|c| [c[0], c[1], c[2], c[3]])
                        .collect::<Vec<_>>()
                        .into_iter();
                    move || it.next().unwrap()
                },
            ));
        }
        let desc_path = dir.join("w.terrainworld.toml");
        std::fs::write(&desc_path, desc).unwrap();

        let table = load_layer_table(&dir.join("cooked")).unwrap();
        assert_eq!(table.len(), 3); // grass, rock, gravel — not the texture, not the notes
        assert!(table[&gravel.1].effective_height > 0.0);
        let cooked = cook_terrain_world(&desc_path).unwrap();
        let bake = cooked.bake.as_ref().expect("palette_dir asks for a bake");
        let world = build_lod_world(tiles, 2).unwrap();
        assert_eq!(bake, &bake_world(&world, &table).unwrap());
        assert_eq!(bake.tiles.iter().flatten().count(), 1);
        assert_eq!(cooked.files.len(), 5 + 2);
        assert_eq!(
            cooked.manifest.lines().last().unwrap().split('\t').count(),
            14
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }

    fn plain_names(files: &[(String, Vec<u8>, u64)]) -> Vec<(String, u64)> {
        files.iter().map(|(f, _, id)| (f.clone(), *id)).collect()
    }

    #[test]
    fn the_layer_table_resolves_cooked_materials_and_terrain_layers() {
        let material = Material {
            base_color: [0.5, 0.25, 1.0, 1.0],
            metallic: 0.75,
            roughness: 0.125,
            ..Material::default()
        }
        .cook();
        // Albedo bytes 255 and 0 alternating in R, 255 in G, 188 in B; height 0 and 255.
        let texels: Vec<u8> = (0..4u8)
            .flat_map(|k| [255 * (k % 2), 255, 188, 255 * (k % 2)])
            .collect();
        let texture = Texture::from_rgba8(2, 2, ColorSpace::Srgb, texels).cook();
        let layer = TerrainLayer {
            material: material.1,
            albedo_height: texture.1,
            uv_scale: [2.0, 2.0],
            height_contrast: 8.0,
        }
        .cook();
        let orphan = TerrainLayer {
            material: 0x1234, // not cooked
            albedo_height: texture.1,
            uv_scale: [1.0, 1.0],
            height_contrast: 1.0,
        }
        .cook();
        let compressed = Texture::from_rgba8(4, 4, ColorSpace::Srgb, vec![9; 64]).cook_with(true);
        let bc_layer = TerrainLayer {
            material: material.1,
            albedo_height: compressed.1,
            uv_scale: [1.0, 1.0],
            height_contrast: 1.0,
        }
        .cook();
        let mut assets = BTreeMap::new();
        for (bytes, id) in [&material, &texture, &layer, &orphan, &compressed, &bc_layer] {
            let (header, payload) = read_header(bytes).unwrap();
            assert_eq!(fnv1a_64(payload), *id);
            assets.insert(*id, (header.asset_kind, payload.to_vec()));
        }
        let table = layer_table(&assets);
        // The material, the layer; not the texture, the orphan, or the BC7-textured layer.
        assert_eq!(table.len(), 2);
        let m = table[&material.1];
        assert_eq!(
            (m.color, m.metallic, m.roughness),
            ([0.5, 0.25, 1.0], 0.75, 0.125)
        );
        assert_eq!(m.effective_height, 0.0);
        let l = table[&layer.1];
        assert_eq!(l.color[0], 0.5 * 0.5);
        assert_eq!(l.color[1], 0.25);
        assert!((l.color[2] - 0.5).abs() < 4e-3); // byte 188 is 50.3 % light
        assert_eq!(
            (l.metallic, l.roughness, l.effective_height),
            (0.75, 0.125, 4.0)
        );
    }
}
