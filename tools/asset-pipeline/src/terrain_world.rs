// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The terrain **world** cook (M19.8d1, ADR-0070): a grid of level-0 heightfield tiles in, the
//! tiles cooked as `rime heightfield` cooks them PLUS every coarser LOD level up to the root cover,
//! and the world manifest the engine's `rime::assets::TerrainWorld` reads (ADR-0069, now with a
//! level column and a geometric error).
//!
//! ## Levels by nested integer subsampling, not filtering
//!
//! A level-L tile covers 2^L × 2^L level-0 tiles with the SAME sample count N and 2^L times the
//! spacing. Its samples are exactly every second sample of its four level-(L−1) children: no
//! averaging, no re-quantisation, the integers copied. So every vertex of a parent IS a vertex of
//! each child beneath it, bit for bit, under the one quantisation every level shares.
//!
//! Why not a low-pass filter, which is what image mip-maps use? A mip-map texel is never drawn on
//! top of its children; a terrain parent is — the renderer (m19.8d2) morphs a child's surface onto
//! the parent's as the camera recedes and stitches a fine tile to a coarse neighbour along their
//! edge. Both are exact only when the coarse vertices are a subset of the fine ones: the morph then
//! only slides the child's EXTRA vertices onto the parent's triangles, and a stitched edge shares
//! every coarse vertex. A filtered parent's vertices sit at heights no child has, so every LOD
//! boundary would crack and need a skirt. The price of subsampling is aliasing (a one-sample spike
//! survives or vanishes depending on its parity) — which the geometric error below measures, so the
//! renderer refines exactly where it matters.
//!
//! Nesting needs the child's last sample to be one the parent takes, i.e. an even cell count:
//! (N − 1) even. That and one quantisation for the whole world are refused otherwise.
//!
//! ## Per-tile LOD metadata
//!
//! * `min_y` / `max_y` bound EVERY level-0 sample beneath the tile, not just the tile's own — a
//!   parent that skipped a peak must still be culled as tall as the peak.
//! * `geometric_error`: the largest |h − s(x, z)| over the level-0 samples beneath the tile, where s
//!   is the tile's own triangulated surface — ADR-0060's fixed (i, j)→(i+1, j+1) diagonal,
//!   barycentric within the triangle — never a bilinear patch, which is a surface nobody draws.
//!   In metres, rounded UP to f32 so it stays a bound, then SATURATED: raised to the largest of
//!   its children's errors, so the error never decreases up the chain (the raw deviation can, and
//!   does on a bumpy world; a refine-when-too-coarse selection needs it monotone). Level 0 is 0.
//!
//! Both live in the world manifest, not the heightfield payload: they describe a tile's relation
//! to the tiles beneath it, which a heightfield does not know about, and a renderer needs them
//! BEFORE deciding to load the tile. So the payload format is unchanged (parents are ordinary v1
//! heightfields) and a parent is usable without any of its descendants loaded.
//!
//! ## The world description: `<name>.terrainworld.toml`
//!
//! In the `*.terrainlayer.toml` style — a strict, hand-parsed TOML subset, `#` comments, unknown
//! and duplicate keys refused:
//!
//! ```text
//! levels = 3                       # LOD levels including level 0 (1 = no parents), 1..=17
//! tile_0_0 = "hills_0_0.png"       # level-0 tile at grid (x, z) = (0, 0): a `rime heightfield`
//! tile_1_0 = "hills_1_0.png"       #   source, relative to this file, with its own sidecar
//! tile_-1_0 = "hills_m1_0.r16"     # coordinates may be negative (a TOML bare key allows `-`)
//! ```
//!
//! One key per tile makes a coordinate named twice a duplicate key — refused by the parser rule
//! every sidecar already has. Every tile must agree on samples, extent and height range (spacing
//! and quantisation, bit for bit), sit where the grid puts it, and match its neighbours' border
//! samples. The root level `levels − 1` must cover the whole world: its span is 2^(levels−1)
//! tiles, the world's bounding box must start on a multiple of the span (else **misaligned**) and
//! be a whole number of spans wide (else **incomplete root cover**), and every 2×2 block at every
//! level must be complete (else **missing child**) — a parent of a partly present block would have
//! to invent the absent samples, and there is no flat fill. Each problem is collected (all of them,
//! not the first), counted by kind, and explained.
//!
//! Parent tiles carry no splat palette. Their appearance is a coarse BAKE (m19.8d3, ADR-0072,
//! `terrain_bake.rs`): with `palette_dir = "cooked"` in the description — the directory holding
//! the palettes' cooked materials, terrain layers and textures — every parent whose level-0 tiles
//! all carry a palette gets two N×N textures (base colour; metallic + roughness), named on its
//! manifest line. Without the key the world cooks exactly as m19.8d1 cooked it.

use std::collections::BTreeMap;
use std::fmt;
use std::path::{Path, PathBuf};

use crate::heightfield::{Heightfield, HeightfieldSidecar, SPLAT_LAYER_COUNT};
use crate::terrain_bake::{bake_world, load_layer_table, LayerTable, WorldBake};
use crate::terrain_layer::strip_comment;
use crate::texture::{cook_single_level, TEXFMT_RGBA8_SRGB, TEXFMT_RGBA8_UNORM};
use crate::PipelineError;

/// The description's file-name suffix: `hills.terrainworld.toml` cooks the world named `hills`.
pub const SIDECAR_SUFFIX: &str = ".terrainworld.toml";

/// The deepest level a world may have — the C++ `kMaxTerrainLevel`.
pub const MAX_LEVEL: u32 = 16;

/// How far a tile's authored origin may sit from where the grid puts it — the C++
/// `kPlacementTolerance` (terrain_world.cpp), so the cook refuses what the engine would.
const PLACEMENT_TOLERANCE: f32 = 1.0e-3;

/// A parsed `<name>.terrainworld.toml`. Tiles in (z, x) order; paths as written.
#[derive(Debug, Clone, PartialEq)]
pub struct TerrainWorldDesc {
    pub levels: u32,
    pub tiles: Vec<(i32, i32, PathBuf)>,
    /// m19.8d3: where the palettes' COOKED assets live (materials, terrain layers and their
    /// textures), relative to the description. Given = bake every parent's appearance.
    pub palette_dir: Option<PathBuf>,
}

impl TerrainWorldDesc {
    pub fn parse(text: &str) -> Result<Self, PipelineError> {
        let bad = |msg: String| PipelineError::Unsupported(format!("terrain world: {msg}"));
        let mut levels = None;
        let mut palette_dir = None;
        let mut tiles: BTreeMap<(i32, i32), PathBuf> = BTreeMap::new();
        for (n, raw) in text.lines().enumerate() {
            let line = strip_comment(raw).trim();
            if line.is_empty() {
                continue;
            }
            let at = |msg: &str| bad(format!("line {}: {msg}", n + 1));
            let (key, value) = line
                .split_once('=')
                .ok_or_else(|| at("expected `key = value`"))?;
            let (key, value) = (key.trim(), value.trim());
            if key == "levels" {
                let l = value
                    .parse::<u32>()
                    .ok()
                    .filter(|l| (1..=MAX_LEVEL + 1).contains(l))
                    .ok_or_else(|| {
                        at(&format!(
                            "`levels` needs an integer in 1..={}",
                            MAX_LEVEL + 1
                        ))
                    })?;
                if levels.replace(l).is_some() {
                    return Err(at("`levels` given twice"));
                }
                continue;
            }
            let quoted = |value: &str| {
                value
                    .strip_prefix('"')
                    .and_then(|v| v.strip_suffix('"'))
                    .filter(|v| !v.is_empty() && !v.contains('"'))
                    .map(PathBuf::from)
            };
            if key == "palette_dir" {
                let dir = quoted(value)
                    .ok_or_else(|| at("`palette_dir` needs a non-empty \"quoted\" path"))?;
                if palette_dir.replace(dir).is_some() {
                    return Err(at("`palette_dir` given twice"));
                }
                continue;
            }
            let coord = key.strip_prefix("tile_").and_then(|c| {
                let (x, z) = c.split_once('_')?;
                Some((z.parse::<i32>().ok()?, x.parse::<i32>().ok()?))
            });
            let Some((z, x)) = coord else {
                return Err(at(&format!(
                    "unknown key `{key}` (expected `levels`, `palette_dir` or `tile_<x>_<z>`)"
                )));
            };
            let path = quoted(value)
                .ok_or_else(|| at(&format!("`{key}` needs a non-empty \"quoted\" path")))?;
            if tiles.insert((z, x), path).is_some() {
                return Err(at(&format!("tile ({x}, {z}) given twice")));
            }
        }
        let levels = levels.ok_or_else(|| bad("missing `levels`".to_string()))?;
        if tiles.is_empty() {
            return Err(bad("names no tiles".to_string()));
        }
        Ok(TerrainWorldDesc {
            levels,
            tiles: tiles.into_iter().map(|((z, x), p)| (x, z, p)).collect(),
            palette_dir,
        })
    }
}

/// Why a world was refused. Every problem is collected, so a cook reports them all at once.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RefusalKind {
    /// (N − 1) is odd and the world asks for parents: every-second-sample nesting cannot land on
    /// the child's last sample.
    OddCellCount,
    /// A tile is not square (a world's tiles share one per-axis sample count).
    NotSquare,
    /// A tile's sample count differs from the world's.
    SizeMismatch,
    /// A tile's extent (hence spacing) differs from the world's, bit for bit.
    SpacingMismatch,
    /// A tile's height range (hence quantisation) differs from the world's, bit for bit.
    QuantisationMismatch,
    /// A tile's origin is not where the grid puts its coordinate (± 1 mm).
    PlacementMismatch,
    /// Two edge neighbours disagree on their shared border samples.
    BorderMismatch,
    /// The world's bounding box does not start on a multiple of the root span.
    Misaligned,
    /// The world's bounding box is not a whole number of root spans: roots cannot cover it.
    IncompleteRootCover,
    /// A 2×2 block at some level has one to three of its four children.
    MissingChild,
    /// m19.8d3: a tile's palette names an id the bake's layer table cannot resolve.
    UnresolvedLayer,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Refusal {
    pub kind: RefusalKind,
    pub detail: String,
}

/// Every reason a world was refused.
#[derive(Debug, Clone, PartialEq)]
pub struct WorldCookError {
    pub refusals: Vec<Refusal>,
}

impl WorldCookError {
    /// How many refusals of `kind` the cook found.
    pub fn count(&self, kind: RefusalKind) -> usize {
        self.refusals.iter().filter(|r| r.kind == kind).count()
    }
}

impl fmt::Display for WorldCookError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "terrain world refused: {} problem(s)",
            self.refusals.len()
        )?;
        for r in &self.refusals {
            write!(f, "\n  - {:?}: {}", r.kind, r.detail)?;
        }
        Ok(())
    }
}

impl From<WorldCookError> for PipelineError {
    fn from(e: WorldCookError) -> Self {
        PipelineError::Unsupported(e.to_string())
    }
}

/// One tile of the chain, any level.
#[derive(Debug, Clone, PartialEq)]
pub struct LodTile {
    pub level: u32,
    pub x: i32,
    pub z: i32,
    pub heightfield: Heightfield,
    /// Smallest / largest level-0 sample beneath the tile (its own samples are a subset).
    pub min_sample: u16,
    pub max_sample: u16,
    /// Metres; 0 for level 0.
    pub geometric_error: f32,
}

/// A validated world: the shared grid and every tile of every level, in (level, z, x) order.
#[derive(Debug, Clone, PartialEq)]
pub struct LodWorld {
    pub samples: u32,
    pub cell_size: [f32; 2],
    pub height_scale: f32,
    pub height_offset: f32,
    /// World position of tile (0, 0)'s origin — the manifest's `grid` origin.
    pub origin: [f32; 3],
    pub levels: u32,
    pub tiles: Vec<LodTile>,
}

impl LodWorld {
    pub fn tile(&self, level: u32, x: i32, z: i32) -> Option<&LodTile> {
        self.tiles
            .iter()
            .find(|t| t.level == level && t.x == x && t.z == z)
    }

    /// One level's pitch (metres per tile), computed exactly as the C++
    /// `TerrainWorldGrid::pitch_x(level)` does: level 0's, scaled by 2^level (exact).
    pub fn pitch(&self, level: u32) -> [f32; 2] {
        let scale = (1u32 << level) as f32;
        let cells = (self.samples - 1) as f32;
        [
            self.cell_size[0] * cells * scale,
            self.cell_size[1] * cells * scale,
        ]
    }

    /// World-space height of a quantised sample, in the engine's f32 order:
    /// `origin.y + (offset + scale * q)`.
    pub fn world_height(&self, q: u16) -> f32 {
        self.origin[1] + (self.height_offset + self.height_scale * f32::from(q))
    }

    /// The manifest text: one `grid` line, then one 10-field `tile` line per tile. `names[k]` is
    /// tile k's `(path, asset id)`.
    pub fn manifest_text(&self, names: &[(String, u64)]) -> String {
        self.manifest_text_with_bakes(names, &[])
    }

    /// As `manifest_text`, with each tile's appearance bake (m19.8d3, ADR-0072): `bakes[k]`, when
    /// present and `Some`, is tile k's `[(path, id); 2]` — base colour, then material — APPENDED to
    /// its line as four more fields. A tile without a bake keeps the 10-field line, so a world
    /// cooked without bakes is byte-identical to m19.8d1's.
    pub fn manifest_text_with_bakes(
        &self,
        names: &[(String, u64)],
        bakes: &[Option<[(String, u64); 2]>],
    ) -> String {
        // f32 `Display` prints the shortest decimal that round-trips, and the engine parses with
        // strtof (round to nearest), so spacing and quantisation come back bit for bit — which
        // check_tile compares.
        let mut s = String::from(
            "# rime terrain world (m19.8d1, ADR-0070) — written by `rime terrain-world`\n",
        );
        s.push_str(&format!(
            "grid\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            self.samples,
            self.cell_size[0],
            self.cell_size[1],
            self.height_scale,
            self.height_offset,
            self.origin[0],
            self.origin[1],
            self.origin[2]
        ));
        for (k, (t, (path, id))) in self.tiles.iter().zip(names).enumerate() {
            s.push_str(&format!(
                "tile\t{}\t{}\t{}\t0\t{}\t{}\t{}\t{id:016x}\t{path}",
                t.level,
                t.x,
                t.z,
                self.world_height(t.min_sample),
                self.world_height(t.max_sample),
                t.geometric_error
            ));
            if let Some(Some([color, material])) = bakes.get(k) {
                s.push_str(&format!(
                    "\t{:016x}\t{}\t{:016x}\t{}",
                    color.1, color.0, material.1, material.0
                ));
            }
            s.push('\n');
        }
        s
    }
}

/// The cooked file name of a tile: `<world>_L<level>_<x>_<z>.rhf`.
pub fn tile_file_name(world: &str, t: &LodTile) -> String {
    format!("{world}_L{}_{}_{}.rhf", t.level, t.x, t.z)
}

/// Round an f64 to the nearest f32 that is not below it, so a cooked error stays an upper bound.
fn round_up_f32(v: f64) -> f32 {
    let f = v as f32;
    if f64::from(f) < v {
        f.next_up()
    } else {
        f
    }
}

/// The height, in quantisation steps, of a tile's triangulated surface at sample-space position
/// (i + u, j + v), u and v in [0, 1]: ADR-0060's DiagonalMinToMax split, barycentric. Triangle A
/// (u >= v) is (i,j), (i+1,j), (i+1,j+1); triangle B (u < v) is (i,j), (i+1,j+1), (i,j+1) — the
/// physics heightfield's convention (engine/physics/src/heightfield.hpp), so the error is measured
/// against the surface that is actually drawn and collided with.
fn surface_q(hf: &Heightfield, i: u32, j: u32, u: f64, v: f64) -> f64 {
    let n = hf.columns as usize;
    let q = |a: u32, b: u32| f64::from(hf.samples[a as usize + n * b as usize]);
    let (q00, q10, q01, q11) = (q(i, j), q(i + 1, j), q(i, j + 1), q(i + 1, j + 1));
    if u >= v {
        q00 + u * (q10 - q00) + v * (q11 - q10)
    } else {
        q00 + v * (q01 - q00) + u * (q11 - q01)
    }
}

/// Validate a level-0 grid and build every parent level up to `levels − 1`. `level0` is
/// `(x, z, heightfield)` per tile.
pub fn build_lod_world(
    mut level0: Vec<(i32, i32, Heightfield)>,
    levels: u32,
) -> Result<LodWorld, WorldCookError> {
    let mut refusals = Vec::new();
    macro_rules! refuse {
        ($kind:expr, $detail:expr $(,)?) => {
            refusals.push(Refusal {
                kind: $kind,
                detail: $detail,
            })
        };
    }
    if level0.is_empty() || !(1..=MAX_LEVEL + 1).contains(&levels) {
        refuse!(
            RefusalKind::IncompleteRootCover,
            format!("{} tiles, {levels} levels: nothing to cover", level0.len()),
        );
        return Err(WorldCookError { refusals });
    }
    level0.sort_by_key(|&(x, z, _)| (z, x));

    // ── 1. Every tile against the first: size, spacing and quantisation, bit for bit ──────────
    let first = &level0[0].2;
    let n = first.columns;
    let (size, range) = (
        [first.sidecar.size_x, first.sidecar.size_z],
        [first.sidecar.height_min, first.sidecar.height_max],
    );
    for (x, z, hf) in &level0 {
        if hf.columns != hf.rows {
            refuse!(
                RefusalKind::NotSquare,
                format!("tile ({x}, {z}) is {}x{} samples", hf.columns, hf.rows),
            );
        } else if hf.columns != n {
            refuse!(
                RefusalKind::SizeMismatch,
                format!(
                    "tile ({x}, {z}) has {} samples per axis, the world {n}",
                    hf.columns
                ),
            );
        }
        let bits = |v: [f32; 2]| v.map(f32::to_bits);
        if bits([hf.sidecar.size_x, hf.sidecar.size_z]) != bits(size) {
            refuse!(
                RefusalKind::SpacingMismatch,
                format!(
                    "tile ({x}, {z}) spans {}x{} m, the world {}x{} m",
                    hf.sidecar.size_x, hf.sidecar.size_z, size[0], size[1]
                ),
            );
        }
        if bits([hf.sidecar.height_min, hf.sidecar.height_max]) != bits(range) {
            refuse!(
                RefusalKind::QuantisationMismatch,
                format!(
                    "tile ({x}, {z}) maps [{}, {}] m, the world [{}, {}] m — one quantisation per \
                     world, so equal integers are equal heights at every level",
                    hf.sidecar.height_min, hf.sidecar.height_max, range[0], range[1]
                ),
            );
        }
    }
    if levels > 1 && !(n - 1).is_multiple_of(2) {
        refuse!(
            RefusalKind::OddCellCount,
            format!(
                "{n} samples per axis is {} cells — odd, so every second sample of a child misses \
                 its last one; use 2^k + 1 samples (or levels = 1)",
                n - 1
            ),
        );
    }
    if !refusals.is_empty() {
        return Err(WorldCookError { refusals });
    }

    // ── 2. The grid, placement and borders ────────────────────────────────────────────────────
    // Spacing exactly as `Heightfield::encode_payload` writes it, pitch exactly as the engine
    // computes it — so the grid line, every tile header and every check agree to the bit.
    let cells = (n - 1) as f32;
    let cell_size = [size[0] / cells, size[1] / cells];
    let pitch = [cell_size[0] * cells, cell_size[1] * cells];
    let (x0, z0, hf0) = (&level0[0].0, &level0[0].1, &level0[0].2);
    let origin = [
        hf0.sidecar.origin[0] - *x0 as f32 * pitch[0],
        hf0.sidecar.origin[1],
        hf0.sidecar.origin[2] - *z0 as f32 * pitch[1],
    ];
    let index: BTreeMap<(i32, i32), usize> = level0
        .iter()
        .enumerate()
        .map(|(k, (x, z, _))| ((*x, *z), k))
        .collect();
    let at = |hf: &Heightfield, i: u32, j: u32| hf.samples[i as usize + n as usize * j as usize];
    for (x, z, hf) in &level0 {
        let want = [
            origin[0] + *x as f32 * pitch[0],
            origin[1],
            origin[2] + *z as f32 * pitch[1],
        ];
        if (0..3).any(|a| (hf.sidecar.origin[a] - want[a]).abs() > PLACEMENT_TOLERANCE) {
            refuse!(
                RefusalKind::PlacementMismatch,
                format!(
                    "tile ({x}, {z}) has origin {:?}, the grid puts it at {want:?}",
                    hf.sidecar.origin
                ),
            );
        }
        // East and north neighbours: the shared column / row must be the same integers (the 8a
        // `edges_match` rule). Checked here because the parent takes ONE of the two copies of a
        // shared edge; if they differed, the other child would not coincide with its parent.
        if let Some(&e) = index.get(&(x + 1, *z)) {
            let east = &level0[e].2;
            if (0..n).any(|j| at(hf, n - 1, j) != at(east, 0, j)) {
                refuse!(
                    RefusalKind::BorderMismatch,
                    format!(
                        "tiles ({x}, {z}) and ({}, {z}) disagree on their shared edge",
                        x + 1
                    ),
                );
            }
        }
        if let Some(&no) = index.get(&(*x, z + 1)) {
            let north = &level0[no].2;
            if (0..n).any(|i| at(hf, i, n - 1) != at(north, i, 0)) {
                refuse!(
                    RefusalKind::BorderMismatch,
                    format!(
                        "tiles ({x}, {z}) and ({x}, {}) disagree on their shared edge",
                        z + 1
                    ),
                );
            }
        }
    }

    // ── 3. Root cover: the bounding box must be a whole, aligned number of root spans ─────────
    let top = levels - 1;
    let span = 1i64 << top;
    let (xs, zs): (Vec<i64>, Vec<i64>) = level0
        .iter()
        .map(|(x, z, _)| (i64::from(*x), i64::from(*z)))
        .unzip();
    for (axis, v) in [("x", &xs), ("z", &zs)] {
        let (lo, hi) = (*v.iter().min().unwrap(), *v.iter().max().unwrap());
        if lo.rem_euclid(span) != 0 {
            refuse!(
                RefusalKind::Misaligned,
                format!(
                    "the world starts at {axis} = {lo}, not a multiple of the level-{top} root span \
                     of {span} tiles — shift the world's coordinates or lower `levels`"
                ),
            );
        }
        if (hi - lo + 1) % span != 0 {
            refuse!(
                RefusalKind::IncompleteRootCover,
                format!(
                    "the world is {} tiles along {axis}, not a multiple of the level-{top} root span \
                     of {span}: roots cannot cover it without padding, which the cook does not \
                     invent — pad the world or lower `levels`",
                    hi - lo + 1
                ),
            );
        }
    }
    if !refusals.is_empty() {
        return Err(WorldCookError { refusals });
    }

    // ── 4. Build upward, block by block ───────────────────────────────────────────────────────
    let height_scale = first.height_scale();
    let height_offset = range[0];
    let mut tiles: Vec<LodTile> = level0
        .into_iter()
        .map(|(x, z, hf)| {
            let min_sample = hf.samples.iter().copied().min().unwrap_or(0);
            let max_sample = hf.samples.iter().copied().max().unwrap_or(0);
            LodTile {
                level: 0,
                x,
                z,
                heightfield: hf,
                min_sample,
                max_sample,
                geometric_error: 0.0,
            }
        })
        .collect();
    let level0_count = tiles.len();
    let mut below: BTreeMap<(i32, i32), usize> = index; // level L−1's (x, z) → index into `tiles`
    let h = (n - 1) / 2;
    for level in 1..levels {
        // Blocks in (z, x) order: a BTreeMap keyed (z, x) of the four child slots.
        let mut blocks: BTreeMap<(i32, i32), [Option<usize>; 4]> = BTreeMap::new();
        for (&(x, z), &k) in &below {
            let slot = (x & 1) as usize + 2 * (z & 1) as usize;
            blocks.entry((z >> 1, x >> 1)).or_insert([None; 4])[slot] = Some(k);
        }
        let mut next = BTreeMap::new();
        for (&(pz, px), kids) in &blocks {
            let present = kids.iter().flatten().count();
            if present < 4 {
                refuse!(
                    RefusalKind::MissingChild,
                    format!(
                        "level-{level} tile ({px}, {pz}) has {present} of its 4 children at level \
                         {} — no flat fill; complete the block",
                        level - 1
                    ),
                );
                continue;
            }
            let kids = kids.map(Option::unwrap);
            // Nesting: parent sample (i, j) lies in child quadrant (qx, qz) = (i >= h, j >= h)
            // (the shared middle row/column taken from the upper child, which the border check
            // made identical to the lower one) at child sample (2(i − qx·h), 2(j − qz·h)).
            let mut samples = Vec::with_capacity((n * n) as usize);
            for j in 0..n {
                for i in 0..n {
                    let (qx, qz) = (u32::from(i >= h), u32::from(j >= h));
                    let child = &tiles[kids[(qx + 2 * qz) as usize]].heightfield;
                    samples.push(at(child, 2 * (i - qx * h), 2 * (j - qz * h)));
                }
            }
            let min_sample = kids.iter().map(|&k| tiles[k].min_sample).min().unwrap();
            let max_sample = kids.iter().map(|&k| tiles[k].max_sample).max().unwrap();
            let scale = (1u32 << level) as f32;
            let lp = [pitch[0] * scale, pitch[1] * scale];
            let sidecar = HeightfieldSidecar {
                // size × 2^L is exact, so the payload's spacing (size / (N − 1)) is bit-identical
                // to level 0's spacing × 2^L — what the engine's check_tile compares.
                size_x: size[0] * scale,
                size_z: size[1] * scale,
                height_min: range[0],
                height_max: range[1],
                origin: [
                    origin[0] + px as f32 * lp[0],
                    origin[1],
                    origin[2] + pz as f32 * lp[1],
                ],
                columns: None,
                rows: None,
                layers: [None; SPLAT_LAYER_COUNT],
            };
            let heightfield =
                Heightfield::new(n, n, samples, sidecar).expect("a parent has the child's shape");
            let mut parent = LodTile {
                level,
                x: px,
                z: pz,
                heightfield,
                min_sample,
                max_sample,
                geometric_error: 0.0,
            };
            // SATURATED: the tile's own deviation, raised to its children's errors. The raw
            // deviation is not monotone up the chain — a coarse surface can happen to pass closer
            // to a spike than a finer one does (measured: the bumpy test world has parents whose
            // raw deviation is below a child's) — and a renderer that refines by "error too big"
            // needs a parent's error to dominate its children's, or it can choose a parent as good
            // enough over a child it would have refined.
            let own = surface_deviation(&parent, n, height_scale, &tiles[..level0_count]);
            parent.geometric_error = kids
                .iter()
                .map(|&k| tiles[k].geometric_error)
                .fold(own, f32::max);
            next.insert((px, pz), tiles.len());
            tiles.push(parent);
        }
        if !refusals.is_empty() {
            return Err(WorldCookError { refusals });
        }
        below = next;
    }
    tiles.sort_by_key(|t| (t.level, t.z, t.x));
    Ok(LodWorld {
        samples: n,
        cell_size,
        height_scale,
        height_offset,
        origin,
        levels,
        tiles,
    })
}

/// The tile's SURFACE DEVIATION: max over the level-0 samples beneath it of the vertical distance
/// to its own triangulated surface, in metres, rounded up to f32. The cooked geometric error is
/// this, saturated by the children's errors (see `build_lod_world`).
///
/// Exact arithmetic: a level-0 sample at offset p (in level-0 sample steps from the tile's corner)
/// sits at tile-sample position p / 2^L — a dyadic rational — so the cell is p >> L, the in-cell
/// fraction (p mod 2^L) / 2^L, and the barycentric height in quantisation steps is exact in f64
/// (16-bit samples times 16-bit fractions need far fewer than 53 bits). Only the final conversion
/// to metres rounds.
fn surface_deviation(parent: &LodTile, n: u32, height_scale: f32, level0: &[LodTile]) -> f32 {
    let level = parent.level;
    let side = 1i32 << level;
    let denom = f64::from(1u32 << level);
    let mask = (1u32 << level) - 1;
    let cell = |p: u32| {
        // The far edge (p = (N−1)·2^L) belongs to the last cell at fraction 1.
        let c = p >> level;
        if c >= n - 1 {
            (n - 2, 1.0)
        } else {
            (c, f64::from(p & mask) / denom)
        }
    };
    let mut worst = 0.0f64;
    for t in level0 {
        let (dx, dz) = (t.x - parent.x * side, t.z - parent.z * side);
        if !(0..side).contains(&dx) || !(0..side).contains(&dz) {
            continue;
        }
        for j in 0..n {
            let (cj, v) = cell(dz as u32 * (n - 1) + j);
            for i in 0..n {
                let (ci, u) = cell(dx as u32 * (n - 1) + i);
                let truth = f64::from(t.heightfield.samples[(i + n * j) as usize]);
                let err = (truth - surface_q(&parent.heightfield, ci, cj, u, v)).abs();
                worst = worst.max(err);
            }
        }
    }
    round_up_f32(worst * f64::from(height_scale))
}

/// A cooked world: every tile's file and the manifest.
#[derive(Debug, Clone)]
pub struct CookedWorld {
    pub name: String,
    pub world: LodWorld,
    /// `(file name, file bytes, asset id)`: one per tile in the world's tile order, then (m19.8d3)
    /// two `.rtex` per baked parent — base colour, material.
    pub files: Vec<(String, Vec<u8>, u64)>,
    pub manifest: String,
    /// m19.8d3: the appearance bake, when the cook was given a layer table.
    pub bake: Option<WorldBake>,
}

impl CookedWorld {
    /// The manifest's file name: `<world>.terrainworld`.
    pub fn manifest_file_name(&self) -> String {
        format!("{}.terrainworld", self.name)
    }
}

/// Cook an in-memory world: every tile's RMA1 bytes plus the manifest naming them.
pub fn cook_lod_world(name: &str, world: LodWorld) -> CookedWorld {
    cook_lod_world_baked(name, world, None).expect("a cook without a bake cannot be refused")
}

/// As `cook_lod_world`, plus (m19.8d3) every parent's appearance bake when `layers` is given:
/// two single-level `.rtex` files per baked parent, named in the manifest's tile line.
pub fn cook_lod_world_baked(
    name: &str,
    world: LodWorld,
    layers: Option<&LayerTable>,
) -> Result<CookedWorld, WorldCookError> {
    let bake = layers.map(|l| bake_world(&world, l)).transpose()?;
    let mut files: Vec<(String, Vec<u8>, u64)> = world
        .tiles
        .iter()
        .map(|t| {
            let (bytes, id) = t.heightfield.cook();
            (tile_file_name(name, t), bytes, id)
        })
        .collect();
    let names: Vec<(String, u64)> = files.iter().map(|(f, _, id)| (f.clone(), *id)).collect();
    let mut bake_names = Vec::new();
    if let Some(bake) = &bake {
        let n = world.samples;
        for (t, b) in world.tiles.iter().zip(&bake.tiles) {
            bake_names.push(b.as_ref().map(|b| {
                let stem = format!("{name}_L{}_{}_{}", t.level, t.x, t.z);
                let color = cook_single_level(n, n, TEXFMT_RGBA8_SRGB, &b.color);
                let material = cook_single_level(n, n, TEXFMT_RGBA8_UNORM, &b.material);
                let names = [
                    (format!("{stem}_bake_color.rtex"), color.1),
                    (format!("{stem}_bake_material.rtex"), material.1),
                ];
                files.push((names[0].0.clone(), color.0, color.1));
                files.push((names[1].0.clone(), material.0, material.1));
                names
            }));
        }
    }
    let manifest = world.manifest_text_with_bakes(&names, &bake_names);
    Ok(CookedWorld {
        name: name.to_string(),
        world,
        files,
        manifest,
        bake,
    })
}

/// Cook a `<name>.terrainworld.toml`: load every level-0 source (each a `rime heightfield` source
/// with its own sidecar, relative to the description), validate, build the chain, cook.
pub fn cook_terrain_world(desc_path: &Path) -> Result<CookedWorld, PipelineError> {
    let file_name = desc_path
        .file_name()
        .and_then(|f| f.to_str())
        .unwrap_or_default();
    let name = file_name
        .strip_suffix(SIDECAR_SUFFIX)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| {
            PipelineError::Unsupported(format!(
                "terrain world {}: the description must be named `<name>{SIDECAR_SUFFIX}`",
                desc_path.display()
            ))
        })?;
    let desc = TerrainWorldDesc::parse(&std::fs::read_to_string(desc_path)?)?;
    let dir = desc_path.parent().unwrap_or(Path::new("."));
    let mut level0 = Vec::with_capacity(desc.tiles.len());
    for (x, z, path) in &desc.tiles {
        level0.push((*x, *z, Heightfield::from_file(&dir.join(path))?));
    }
    let layers = desc
        .palette_dir
        .as_ref()
        .map(|d| load_layer_table(&dir.join(d)))
        .transpose()?;
    Ok(cook_lod_world_baked(
        name,
        build_lod_world(level0, desc.levels)?,
        layers.as_ref(),
    )?)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A deterministic hash of a global sample position: "random" heights that still agree along
    /// every shared tile edge, because they are a function of the world position.
    fn bumpy(gx: i64, gz: i64) -> u16 {
        let mut h = (gx as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15)
            ^ (gz as u64).wrapping_mul(0xc2b2_ae3d_27d4_eb4f);
        h ^= h >> 29;
        h = h.wrapping_mul(0xbf58_476d_1ce4_e5b9);
        h ^= h >> 32;
        (h % 60000) as u16 + 1000
    }

    fn planar(gx: i64, gz: i64) -> u16 {
        (20000 + 37 * gx - 11 * gz) as u16
    }

    const SIZE: f32 = 8.0; // metres per level-0 tile
    const RANGE: [f32; 2] = [-10.0, 55.535];

    fn sidecar(origin: [f32; 3]) -> HeightfieldSidecar {
        HeightfieldSidecar {
            size_x: SIZE,
            size_z: SIZE,
            height_min: RANGE[0],
            height_max: RANGE[1],
            origin,
            columns: None,
            rows: None,
            layers: [None; SPLAT_LAYER_COUNT],
        }
    }

    /// Level-0 tiles for the coordinates in `coords`, N samples each, heights `f(global x, z)`.
    fn tiles_of(
        coords: &[(i32, i32)],
        n: u32,
        f: fn(i64, i64) -> u16,
    ) -> Vec<(i32, i32, Heightfield)> {
        coords
            .iter()
            .map(|&(x, z)| {
                let mut s = Vec::new();
                for j in 0..n {
                    for i in 0..n {
                        s.push(f(
                            i64::from(x) * i64::from(n - 1) + i64::from(i),
                            i64::from(z) * i64::from(n - 1) + i64::from(j),
                        ));
                    }
                }
                let o = [x as f32 * SIZE, 2.5, z as f32 * SIZE];
                (x, z, Heightfield::new(n, n, s, sidecar(o)).unwrap())
            })
            .collect()
    }

    fn square(w: i32, h: i32) -> Vec<(i32, i32)> {
        (0..h).flat_map(|z| (0..w).map(move |x| (x, z))).collect()
    }

    /// Heights in metres (f64) of a global level-0 sample — the brute force's own dequantisation.
    fn metres(world: &LodWorld, q: u16) -> f64 {
        f64::from(world.height_offset) + f64::from(world.height_scale) * f64::from(q)
    }

    /// Brute force for (b), deliberately different maths from the cook: world-space positions in
    /// metres, the containing triangle found by comparing positions, and the height read off the
    /// triangle's PLANE (normal from a cross product) rather than an interpolation in sample space.
    fn brute_error(world: &LodWorld, t: &LodTile, f: fn(i64, i64) -> u16) -> f64 {
        let n = i64::from(world.samples);
        let side = 1i64 << t.level;
        let cell0 = f64::from(SIZE) / (n - 1) as f64;
        let cell = cell0 * side as f64;
        let base = (
            i64::from(t.x) * side * (n - 1),
            i64::from(t.z) * side * (n - 1),
        );
        let p = |i: i64, j: i64| {
            let q = t.heightfield.samples[(i + n * j) as usize];
            [i as f64 * cell, metres(world, q), j as f64 * cell]
        };
        let mut worst = 0.0f64;
        for gz in base.1..=base.1 + side * (n - 1) {
            for gx in base.0..=base.0 + side * (n - 1) {
                let (x, z) = ((gx - base.0) as f64 * cell0, (gz - base.1) as f64 * cell0);
                let (ci, cj) = (
                    ((x / cell).floor() as i64).min(n - 2),
                    ((z / cell).floor() as i64).min(n - 2),
                );
                let (u, v) = (x - ci as f64 * cell, z - cj as f64 * cell);
                let tri = if u >= v {
                    [p(ci, cj), p(ci + 1, cj), p(ci + 1, cj + 1)]
                } else {
                    [p(ci, cj), p(ci + 1, cj + 1), p(ci, cj + 1)]
                };
                let e1 = [
                    tri[1][0] - tri[0][0],
                    tri[1][1] - tri[0][1],
                    tri[1][2] - tri[0][2],
                ];
                let e2 = [
                    tri[2][0] - tri[0][0],
                    tri[2][1] - tri[0][1],
                    tri[2][2] - tri[0][2],
                ];
                let nrm = [
                    e1[1] * e2[2] - e1[2] * e2[1],
                    e1[2] * e2[0] - e1[0] * e2[2],
                    e1[0] * e2[1] - e1[1] * e2[0],
                ];
                let y = tri[0][1] - (nrm[0] * (x - tri[0][0]) + nrm[2] * (z - tri[0][2])) / nrm[1];
                worst = worst.max((metres(world, f(gx, gz)) - y).abs());
            }
        }
        worst
    }

    #[test]
    fn a_four_by_four_world_nests_exactly_up_to_a_single_root() {
        // (a) Every parent sample equals the level-0 sample at that world position — the global
        // function — AND the level-(L−1) child sample there, at every level.
        let n = 9;
        let world = build_lod_world(tiles_of(&square(4, 4), n, bumpy), 3).unwrap();
        assert_eq!(world.tiles.len(), 16 + 4 + 1);
        let (n64, nn) = (i64::from(n), n as usize);
        for t in world.tiles.iter().filter(|t| t.level > 0) {
            let side = 1i64 << t.level;
            for j in 0..n64 {
                for i in 0..n64 {
                    let q = t.heightfield.samples[(i + n64 * j) as usize];
                    let gx = i64::from(t.x) * side * (n64 - 1) + i * side;
                    let gz = i64::from(t.z) * side * (n64 - 1) + j * side;
                    assert_eq!(
                        q,
                        bumpy(gx, gz),
                        "level {} ({}, {}) sample ({i}, {j})",
                        t.level,
                        t.x,
                        t.z
                    );
                    // The child holding this position, and its sample there.
                    let half = side / 2;
                    let (cx, cz) = (
                        gx.div_euclid(half * (n64 - 1)),
                        gz.div_euclid(half * (n64 - 1)),
                    );
                    let (cx, cz) = (
                        cx.min(i64::from(t.x) * 2 + 1),
                        cz.min(i64::from(t.z) * 2 + 1),
                    );
                    let c = world.tile(t.level - 1, cx as i32, cz as i32).unwrap();
                    let (li, lj) = (
                        (gx - cx * half * (n64 - 1)) / half,
                        (gz - cz * half * (n64 - 1)) / half,
                    );
                    assert_eq!(q, c.heightfield.samples[li as usize + nn * lj as usize]);
                }
            }
        }
        let root = world.tile(2, 0, 0).unwrap();
        assert_eq!(root.heightfield.splat, None); // parents carry no palette (8d3)
    }

    #[test]
    fn spacing_doubles_bit_exactly_and_quantisation_is_shared() {
        let world = build_lod_world(tiles_of(&square(4, 4), 5, bumpy), 3).unwrap();
        let l0 = world.tile(0, 0, 0).unwrap().heightfield.encode_payload();
        let f32_at = |p: &[u8], o: usize| f32::from_le_bytes(p[o..o + 4].try_into().unwrap());
        for t in &world.tiles {
            let p = t.heightfield.encode_payload();
            let scale = (1u32 << t.level) as f32;
            assert_eq!(
                f32_at(&p, 12).to_bits(),
                (f32_at(&l0, 12) * scale).to_bits()
            );
            assert_eq!(
                f32_at(&p, 16).to_bits(),
                (f32_at(&l0, 16) * scale).to_bits()
            );
            assert_eq!(f32_at(&p, 32).to_bits(), f32_at(&l0, 32).to_bits()); // scale
            assert_eq!(f32_at(&p, 36).to_bits(), f32_at(&l0, 36).to_bits()); // offset
                                                                             // Origin = the grid's dyadic position, computed as the engine does.
            let pitch = world.pitch(t.level);
            assert_eq!(f32_at(&p, 20), world.origin[0] + t.x as f32 * pitch[0]);
            assert_eq!(f32_at(&p, 28), world.origin[2] + t.z as f32 * pitch[1]);
        }
    }

    #[test]
    fn geometric_error_matches_a_brute_force_and_is_zero_for_a_plane() {
        // (b) Planar world: every triangulation of a plane is the plane, so every error is 0.
        let planar_world = build_lod_world(tiles_of(&square(4, 4), 9, planar), 3).unwrap();
        for t in &planar_world.tiles {
            assert_eq!(
                t.geometric_error, 0.0,
                "planar level {} ({}, {})",
                t.level, t.x, t.z
            );
            assert!(brute_error(&planar_world, t, planar) < 1e-9);
        }
        // Bumpy world: the cooked error is the brute-force deviation saturated by the children's
        // cooked errors — positive, and monotone up the chain.
        let world = build_lod_world(tiles_of(&square(4, 4), 9, bumpy), 3).unwrap();
        let mut saturated = 0;
        for t in &world.tiles {
            let brute = brute_error(&world, t, bumpy);
            if t.level == 0 {
                assert_eq!(t.geometric_error, 0.0);
                assert!(brute < 1e-9, "a tile is exact on its own samples");
                continue;
            }
            // The cook rounds UP to f32 (a bound): never below the brute force, and the f32 just
            // beneath it is (the brute force's plane maths differs only in the last f64 bits).
            let own = round_up_f32(brute);
            let own_ok =
                |e: f32| brute <= f64::from(e) + 1e-9 && f64::from(e.next_down()) <= brute + 1e-9;
            let kids: Vec<f32> = (0..4)
                .map(|k| {
                    world
                        .tile(t.level - 1, t.x * 2 + k % 2, t.z * 2 + k / 2)
                        .unwrap()
                        .geometric_error
                })
                .collect();
            let max_kid = kids.iter().copied().fold(0.0f32, f32::max);
            let e = t.geometric_error;
            assert!(e > 0.0);
            assert!(e >= max_kid, "monotone up the chain");
            if max_kid > own {
                saturated += 1;
                assert_eq!(e, max_kid, "level {} ({}, {})", t.level, t.x, t.z);
            } else {
                assert!(
                    own_ok(e),
                    "level {} ({}, {}): cooked {e} brute {brute}",
                    t.level,
                    t.x,
                    t.z
                );
            }
        }
        // The saturation is load-bearing on this world: without it the chain would not be monotone.
        assert!(saturated > 0);
    }

    #[test]
    fn min_max_bound_every_descendant_sample_and_are_tight() {
        // (c) A parent's bounds cover every level-0 sample beneath it — and are attained, so they
        // are the descendants' extremes, not a padded guess.
        let world = build_lod_world(tiles_of(&square(4, 4), 9, bumpy), 3).unwrap();
        for t in world.tiles.iter().filter(|t| t.level > 0) {
            let side = 1i32 << t.level;
            let mut lo = u16::MAX;
            let mut hi = 0u16;
            for c in world.tiles.iter().filter(|c| c.level == 0) {
                if (c.x - t.x * side) >= 0
                    && (c.x - t.x * side) < side
                    && (c.z - t.z * side) >= 0
                    && (c.z - t.z * side) < side
                {
                    for &q in &c.heightfield.samples {
                        assert!(world.world_height(q) >= world.world_height(t.min_sample));
                        assert!(world.world_height(q) <= world.world_height(t.max_sample));
                        lo = lo.min(q);
                        hi = hi.max(q);
                    }
                }
            }
            assert_eq!((t.min_sample, t.max_sample), (lo, hi));
        }
        // And the parent's own samples genuinely miss the extremes somewhere, so "own samples only"
        // would be a different (wrong) answer — the (c) falsification is not vacuous on this world.
        let differs = world.tiles.iter().filter(|t| t.level > 0).any(|t| {
            let own_min = t.heightfield.samples.iter().copied().min().unwrap();
            let own_max = t.heightfield.samples.iter().copied().max().unwrap();
            own_min > t.min_sample || own_max < t.max_sample
        });
        assert!(differs);
    }

    #[test]
    fn a_root_grid_covers_a_world_that_is_not_a_power_of_two_square() {
        let world = build_lod_world(tiles_of(&square(4, 2), 5, bumpy), 2).unwrap();
        let roots: Vec<_> = world
            .tiles
            .iter()
            .filter(|t| t.level == 1)
            .map(|t| (t.x, t.z))
            .collect();
        assert_eq!(roots, vec![(0, 0), (1, 0)]);
        // Negative coordinates nest by FLOOR division: tiles -2 and -1 share parent -1.
        let coords: Vec<_> = (-2..2).flat_map(|x| [(x, -2), (x, -1)]).collect();
        let w = build_lod_world(tiles_of(&coords, 5, bumpy), 2).unwrap();
        assert!(w.tile(1, -1, -1).is_some() && w.tile(1, 0, -1).is_some());
        assert_eq!(w.origin, [0.0, 2.5, 0.0]);
    }

    #[test]
    fn levels_one_is_the_m19_8a_world_and_needs_no_even_cell_count() {
        let world = build_lod_world(tiles_of(&square(3, 1), 8, bumpy), 1).unwrap();
        assert_eq!(world.tiles.len(), 3);
        assert!(world
            .tiles
            .iter()
            .all(|t| t.level == 0 && t.geometric_error == 0.0));
    }

    fn refused(tiles: Vec<(i32, i32, Heightfield)>, levels: u32) -> WorldCookError {
        build_lod_world(tiles, levels).unwrap_err()
    }

    #[test]
    fn every_refusal_is_counted_and_explained() {
        // (d) Odd (N − 1).
        let e = refused(tiles_of(&square(2, 2), 8, bumpy), 2);
        assert_eq!(
            (e.count(RefusalKind::OddCellCount), e.refusals.len()),
            (1, 1)
        );
        assert!(e.to_string().contains("7 cells"), "{e}");
        // Quantisation mismatch: one tile maps a different height range.
        let mut t = tiles_of(&square(2, 2), 5, bumpy);
        t[3].2.sidecar.height_max = 99.0;
        let e = refused(t, 2);
        assert_eq!(
            (e.count(RefusalKind::QuantisationMismatch), e.refusals.len()),
            (1, 1)
        );
        // Spacing and size mismatches.
        let mut t = tiles_of(&square(2, 2), 5, bumpy);
        t[1].2.sidecar.size_x = 9.0;
        assert_eq!(refused(t, 2).count(RefusalKind::SpacingMismatch), 1);
        let mut t = tiles_of(&square(2, 1), 5, bumpy);
        t.extend(tiles_of(&[(0, 1)], 9, bumpy));
        assert_eq!(refused(t, 1).count(RefusalKind::SizeMismatch), 1);
        // Missing child: a 4x4 world (aligned, whole spans) with one tile removed.
        let coords: Vec<_> = square(4, 4).into_iter().filter(|&c| c != (1, 1)).collect();
        let e = refused(tiles_of(&coords, 5, bumpy), 2);
        assert_eq!(
            (e.count(RefusalKind::MissingChild), e.refusals.len()),
            (1, 1)
        );
        assert!(e.to_string().contains("3 of its 4 children"), "{e}");
        // ...and the same hole caught one level up when its whole block is gone.
        let coords: Vec<_> = square(4, 4)
            .into_iter()
            .filter(|&(x, z)| x >= 2 || z >= 2)
            .collect();
        let e = refused(tiles_of(&coords, 5, bumpy), 3);
        assert_eq!(e.count(RefusalKind::MissingChild), 1);
        assert!(e.to_string().contains("level-2 tile (0, 0) has 3"), "{e}");
        // Incomplete root cover: 3 tiles wide cannot be covered by 2-wide roots.
        let e = refused(tiles_of(&square(3, 2), 5, bumpy), 2);
        assert_eq!(
            (e.count(RefusalKind::IncompleteRootCover), e.refusals.len()),
            (1, 1)
        );
        // Misaligned: the right size, starting on an odd tile.
        let coords: Vec<_> = square(2, 2).into_iter().map(|(x, z)| (x + 1, z)).collect();
        let e = refused(tiles_of(&coords, 5, bumpy), 2);
        assert_eq!((e.count(RefusalKind::Misaligned), e.refusals.len()), (1, 1));
        // Placement and border.
        let mut t = tiles_of(&square(2, 2), 5, bumpy);
        t[2].2.sidecar.origin[0] += 0.01;
        assert_eq!(refused(t, 2).count(RefusalKind::PlacementMismatch), 1);
        let mut t = tiles_of(&square(2, 2), 5, bumpy);
        t[0].2.samples[4] ^= 1; // (N−1, 0): tile (0, 0)'s east edge
        assert_eq!(refused(t, 2).count(RefusalKind::BorderMismatch), 1);
        // Not square.
        let s = vec![0u16; 5 * 3];
        let t = vec![(0, 0, Heightfield::new(5, 3, s, sidecar([0.0; 3])).unwrap())];
        assert_eq!(refused(t, 1).count(RefusalKind::NotSquare), 1);
    }

    #[test]
    fn the_manifest_lists_every_level_with_ten_fields() {
        let cooked = cook_lod_world(
            "w",
            build_lod_world(tiles_of(&square(2, 2), 5, bumpy), 2).unwrap(),
        );
        let lines: Vec<&str> = cooked
            .manifest
            .lines()
            .filter(|l| !l.starts_with('#'))
            .collect();
        assert_eq!(lines.len(), 1 + 5);
        assert_eq!(lines[0].split('\t').count(), 9);
        assert!(lines[0].starts_with("grid\t5\t2\t2\t"));
        for (line, (file, _, id)) in lines[1..].iter().zip(&cooked.files) {
            let f: Vec<&str> = line.split('\t').collect();
            assert_eq!(f.len(), 10);
            assert_eq!(f[9], file);
            assert_eq!(u64::from_str_radix(f[8], 16).unwrap(), *id);
        }
        assert!(lines[5].starts_with("tile\t1\t0\t0\t0\t"));
        assert_eq!(cooked.files[4].0, "w_L1_0_0.rhf");
        // Cooking twice is byte-identical, manifest included.
        let again = cook_lod_world(
            "w",
            build_lod_world(tiles_of(&square(2, 2), 5, bumpy), 2).unwrap(),
        );
        assert_eq!(again.manifest, cooked.manifest);
        assert!(again
            .files
            .iter()
            .zip(&cooked.files)
            .all(|(a, b)| a.1 == b.1));
    }

    #[test]
    fn palette_dir_is_optional_quoted_and_given_once() {
        // m19.8d3: the key that asks for the appearance bake.
        let d = TerrainWorldDesc::parse("levels = 1\ntile_0_0 = \"a.r16\"").unwrap();
        assert_eq!(d.palette_dir, None);
        let d = TerrainWorldDesc::parse(
            "levels = 1\npalette_dir = \"../cooked\" # where the .rtl live\ntile_0_0 = \"a.r16\"",
        )
        .unwrap();
        assert_eq!(d.palette_dir, Some(PathBuf::from("../cooked")));
        for bad in [
            "palette_dir = cooked",
            "palette_dir = \"\"",
            "palette_dir = \"a\"\npalette_dir = \"b\"",
        ] {
            let text = format!("levels = 1\ntile_0_0 = \"a.r16\"\n{bad}");
            assert!(TerrainWorldDesc::parse(&text).is_err(), "{bad}");
        }
    }

    #[test]
    fn the_description_is_strict() {
        let d = TerrainWorldDesc::parse(
            "levels = 2 # two\ntile_1_0 = \"b.png\"\ntile_0_0 = \"a #1.png\"\ntile_-1_-3 = \"c.r16\"\n",
        )
        .unwrap();
        assert_eq!(d.levels, 2);
        assert_eq!(
            d.tiles,
            vec![
                (-1, -3, PathBuf::from("c.r16")),
                (0, 0, PathBuf::from("a #1.png")),
                (1, 0, PathBuf::from("b.png"))
            ]
        );
        for bad in [
            "tile_0_0 = \"a.png\"\n",         // no levels
            "levels = 1\n",                   // no tiles
            "levels = 0\ntile_0_0 = \"a\"\n", // levels out of range
            "levels = 18\ntile_0_0 = \"a\"\n",
            "levels = 1\nlevels = 1\ntile_0_0 = \"a\"\n", // duplicate key
            "levels = 1\ntile_0_0 = \"a\"\ntile_0_0 = \"b\"\n", // duplicate coordinate
            "levels = 1\ntile_0 = \"a\"\n",               // malformed key
            "levels = 1\ntile_0_x = \"a\"\n",
            "levels = 1\ntile_0_0 = a\n",                  // unquoted
            "levels = 1\nmystery = 1\ntile_0_0 = \"a\"\n", // unknown key
        ] {
            assert!(TerrainWorldDesc::parse(bad).is_err(), "{bad:?}");
        }
    }
}
