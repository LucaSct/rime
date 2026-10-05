// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The terrain **heightfield** cook (M19.1, ADR-0060-m19.1-heightfield): a 16-bit grayscale height
//! map plus a small sidecar of world dimensions in, one RMA1 `Heightfield` payload out. The C++
//! reader is `engine/assets` (`read_heightfield`); the byte layout is in `FORMAT.md`.
//!
//! ## Why the cook does (almost) nothing to the samples
//!
//! Terrain is authored as 16-bit integers — every terrain tool exports 16-bit grayscale PNG or raw
//! — and the cooked format stores 16-bit integers with a `height_scale`/`height_offset` pair in the
//! header (`height = offset + scale * sample`). So the cook **copies the samples verbatim**: it
//! never converts to float and back, which makes it exact (the only quantisation in the pipeline
//! is the one the authoring tool already made) and deterministic by construction (no float
//! arithmetic touches the payload's bulk). The sidecar decides what the integers *mean*: the
//! vertical range `[height_min, height_max]` maps linearly onto `[0, 65535]`, so the height step is
//! `(height_max - height_min) / 65535` and the worst-case vertical rounding error of a surface
//! quantised this way is half a step.
//!
//! ## The sidecar
//!
//! A heightmap image says nothing about metres, so a few lines of `key = value` (a TOML subset — no
//! tables, no strings; `#` comments) sit beside it, named after the source with a `.toml`
//! extension (`terrain.png` → `terrain.toml`):
//!
//! ```text
//! size_x = 512.0      # metres covered along X (first sample to last)
//! size_z = 512.0      # metres covered along Z
//! height_min = -20.0  # metres at sample value 0
//! height_max = 180.0  # metres at sample value 65535
//! origin_x = 0.0      # world position of the grid's (0, 0) sample — optional, default 0
//! origin_y = 0.0
//! origin_z = 0.0
//! columns = 513       # RAW sources only: samples per row (a PNG carries its own size)
//! rows = 513
//! layer0 = 0x1f2e3d4c5b6a7988  # optional: material AssetIds for the splat palette (M19.4)
//! layer1 = 0x0123456789abcdef  # layer0..layer3; layer0 is required once any is given
//! ```
//!
//! A hand-rolled parser rather than a TOML dependency: the format is a handful of numbers, and a
//! new crate for a handful of numbers is a supply-chain cost with no reader to show for it.
//!
//! ## Splat materials (M19.4, ADR-0063)
//!
//! A tile may blend up to four materials. The palette is the sidecar's `layer0..layer3`, each a
//! material **AssetId** in `0x` hex — not a path, for the same reason `material.rs` references
//! textures by id: a reference in a cooked file is the content hash of a *cooked* asset, so the
//! terrain names exactly the bytes it needs and the engine resolves each id to one shared upload.
//! An omitted slot is "unused" (id 0). The per-texel weights come from a sibling
//! `<source stem>.splat.png` (`terrain.png` -> `terrain.splat.png`), 8-bit RGBA, R/G/B/A being
//! layer 0/1/2/3, at its OWN resolution (a splat map is usually coarser or finer than the height
//! grid). The cook normalizes each texel to sum to exactly 255 (see [`normalize_weights`]) because
//! the engine's reader refuses anything else. No `layerN` key means a v1 payload, byte-identical
//! to what this cook always emitted; a `.splat.png` with no palette is refused rather than ignored.

use std::path::Path;

use crate::cooked::{wrap_container, ByteWriter, ASSET_KIND_HEIGHTFIELD, HEIGHTFIELD_SCHEMA_HASH};
use crate::PipelineError;

/// The payload's own version (the first field) — the C++ `kHeightfieldPayloadVersion`: the newest
/// version the format defines, which carries the splat-material block.
pub const HEIGHTFIELD_PAYLOAD_VERSION: u32 = 2;

/// Version 1: no splat block. Still emitted for a heightfield without a palette, so existing
/// cooked terrain (and its content id) does not change merely because the format grew.
pub const HEIGHTFIELD_PAYLOAD_VERSION_V1: u32 = 1;

/// Palette size: four layers fit one RGBA8 fetch (the C++ `HeightfieldAsset::kLayerCount`).
pub const SPLAT_LAYER_COUNT: usize = 4;

/// The only triangulation v1 cooks: each cell split along the diagonal from sample (i, j) to
/// (i+1, j+1). Part of the format because physics and rendering must split non-planar cells the
/// same way (see `engine/assets/include/rime/assets/heightfield_asset.hpp`).
pub const TRIANGULATION_DIAGONAL_MIN_TO_MAX: u32 = 0;

/// Per-axis ceiling, matching the C++ reader's `kMaxHeightfieldSamplesPerAxis`: the cook refuses
/// what the engine would refuse, so an oversize tile fails at cook time with a message instead of
/// at load time with an error code.
pub const MAX_SAMPLES_PER_AXIS: u32 = 16385;

/// The world-space meaning of a height map: its extent, vertical range and placement.
#[derive(Debug, Clone, PartialEq)]
pub struct HeightfieldSidecar {
    pub size_x: f32,
    pub size_z: f32,
    pub height_min: f32,
    pub height_max: f32,
    pub origin: [f32; 3],
    /// Only meaningful (and only required) for a raw `.r16` source.
    pub columns: Option<u32>,
    pub rows: Option<u32>,
    /// The splat palette: material AssetIds, `None` = unused slot (written as id 0).
    pub layers: [Option<u64>; SPLAT_LAYER_COUNT],
}

impl HeightfieldSidecar {
    /// Parse the `key = value` sidecar. Unknown keys and duplicate keys are errors, not warnings:
    /// a typo'd `hieght_max` silently falling back to a default is a terrain 100 m in the wrong
    /// place that nothing ever reports.
    pub fn parse(text: &str) -> Result<Self, PipelineError> {
        let bad = |msg: String| PipelineError::Unsupported(format!("heightfield sidecar: {msg}"));
        let mut size_x = None;
        let mut size_z = None;
        let mut height_min = None;
        let mut height_max = None;
        let mut origin = [None, None, None];
        let mut columns = None;
        let mut rows = None;
        let mut layers = [None; SPLAT_LAYER_COUNT];
        for (n, raw) in text.lines().enumerate() {
            let line = raw.split('#').next().unwrap_or("").trim();
            if line.is_empty() {
                continue;
            }
            let (key, value) = line
                .split_once('=')
                .ok_or_else(|| bad(format!("line {}: expected `key = value`", n + 1)))?;
            let key = key.trim();
            let value = value.trim();
            let float = || {
                value
                    .parse::<f32>()
                    .ok()
                    .filter(|v| v.is_finite())
                    .ok_or_else(|| bad(format!("line {}: `{key}` needs a finite number", n + 1)))
            };
            let int = || {
                value
                    .parse::<u32>()
                    .map_err(|_| bad(format!("line {}: `{key}` needs an integer", n + 1)))
            };
            let slot_f = match key {
                "size_x" => Some(&mut size_x),
                "size_z" => Some(&mut size_z),
                "height_min" => Some(&mut height_min),
                "height_max" => Some(&mut height_max),
                "origin_x" => Some(&mut origin[0]),
                "origin_y" => Some(&mut origin[1]),
                "origin_z" => Some(&mut origin[2]),
                _ => None,
            };
            if let Some(slot) = slot_f {
                if slot.replace(float()?).is_some() {
                    return Err(bad(format!("line {}: `{key}` given twice", n + 1)));
                }
                continue;
            }
            if let Some(k) = key
                .strip_prefix("layer")
                .and_then(|d| d.parse::<usize>().ok())
                .filter(|&k| k < SPLAT_LAYER_COUNT && key.len() == 6)
            {
                // Hex only: an AssetId is a 64-bit content hash, printed as hex everywhere (`rime
                // heightfield` prints `id {:016x}`); a decimal here would be a typo that names
                // some other asset. 0 is the "unused" sentinel, so naming it is a mistake.
                let id = value
                    .strip_prefix("0x")
                    .filter(|h| !h.is_empty())
                    .and_then(|h| u64::from_str_radix(h, 16).ok())
                    .filter(|&id| id != 0)
                    .ok_or_else(|| {
                        bad(format!(
                            "line {}: `{key}` needs a nonzero material AssetId in 0x hex",
                            n + 1
                        ))
                    })?;
                if layers[k].replace(id).is_some() {
                    return Err(bad(format!("line {}: `{key}` given twice", n + 1)));
                }
                continue;
            }
            let slot_i = match key {
                "columns" => &mut columns,
                "rows" => &mut rows,
                _ => return Err(bad(format!("line {}: unknown key `{key}`", n + 1))),
            };
            if slot_i.replace(int()?).is_some() {
                return Err(bad(format!("line {}: `{key}` given twice", n + 1)));
            }
        }
        let need = |v: Option<f32>, k: &str| v.ok_or_else(|| bad(format!("missing `{k}`")));
        let sc = HeightfieldSidecar {
            size_x: need(size_x, "size_x")?,
            size_z: need(size_z, "size_z")?,
            height_min: need(height_min, "height_min")?,
            height_max: need(height_max, "height_max")?,
            origin: [
                origin[0].unwrap_or(0.0),
                origin[1].unwrap_or(0.0),
                origin[2].unwrap_or(0.0),
            ],
            columns,
            rows,
            layers,
        };
        if sc.layers.iter().any(Option::is_some) && sc.layers[0].is_none() {
            return Err(bad(
                "`layer0` is required once any layer is given (slot 0 is the base material)"
                    .to_string(),
            ));
        }
        if sc.size_x <= 0.0 || sc.size_z <= 0.0 {
            return Err(bad("size_x and size_z must be positive".to_string()));
        }
        if sc.height_max <= sc.height_min {
            return Err(bad("height_max must be above height_min".to_string()));
        }
        Ok(sc)
    }
}

/// A heightfield ready to cook: the grid of samples and what they mean.
#[derive(Debug, Clone, PartialEq)]
pub struct Heightfield {
    pub columns: u32,
    pub rows: u32,
    /// Row-major, x fastest: sample (i, j) is `samples[i + columns * j]`.
    pub samples: Vec<u16>,
    pub sidecar: HeightfieldSidecar,
    /// The normalized splat weights, when the sidecar names a palette (payload v2).
    pub splat: Option<SplatWeights>,
}

/// A validated splat-weight grid: every texel already sums to exactly 255.
#[derive(Debug, Clone, PartialEq)]
pub struct SplatWeights {
    pub columns: u32,
    pub rows: u32,
    /// `columns * rows * 4` bytes, row-major, x fastest, the four layers interleaved per texel.
    pub weights: Vec<u8>,
}

/// Normalize one texel's raw weights to sum to exactly 255 by **largest-remainder apportionment**
/// (the Hamilton method used to split parliamentary seats), in integers only.
///
/// Why not round-to-nearest: rounding each channel independently drifts the sum — an even
/// two-way split is 127.5 each, which rounds to 128 twice = 256 — and the engine's reader refuses
/// a texel that does not sum to 255, so a "close enough" cook would be an unloadable file.
/// Instead: take each channel's floor of `w*255/S`, then hand the leftover units (`255 - sum of
/// floors`, always less than the channel count) one each to the channels with the largest
/// fractional remainders, ties to the lowest channel index so the result is deterministic.
/// Integer arithmetic means no float rounding can differ between hosts.
/// A texel that already sums to 255 comes out unchanged (every remainder is 0, deficit is 0).
///
/// `raw` must have a positive sum; the caller refuses an all-zero texel first.
pub fn normalize_weights(raw: [u32; SPLAT_LAYER_COUNT]) -> [u8; SPLAT_LAYER_COUNT] {
    let total: u32 = raw.iter().sum();
    debug_assert!(total > 0, "an all-zero texel cannot be normalized");
    let mut out = [0u32; SPLAT_LAYER_COUNT];
    let mut rem = [0u32; SPLAT_LAYER_COUNT];
    for k in 0..SPLAT_LAYER_COUNT {
        out[k] = raw[k] * 255 / total;
        rem[k] = raw[k] * 255 % total;
    }
    let deficit = 255 - out.iter().sum::<u32>();
    // Channel order by remainder descending, then index ascending: a stable sort on the
    // descending remainder keeps the lowest index first among ties.
    let mut order = [0usize, 1, 2, 3];
    order.sort_by(|&a, &b| rem[b].cmp(&rem[a]));
    for &k in order.iter().take(deficit as usize) {
        out[k] += 1;
    }
    out.map(|v| v as u8)
}

impl Heightfield {
    /// Build from in-memory samples (the path the tests and the file loaders share).
    pub fn new(
        columns: u32,
        rows: u32,
        samples: Vec<u16>,
        sidecar: HeightfieldSidecar,
    ) -> Result<Self, PipelineError> {
        if columns < 2 || rows < 2 {
            return Err(PipelineError::Unsupported(format!(
                "heightfield: {columns}x{rows} samples is not a surface (need at least 2x2)"
            )));
        }
        if columns > MAX_SAMPLES_PER_AXIS || rows > MAX_SAMPLES_PER_AXIS {
            return Err(PipelineError::Unsupported(format!(
                "heightfield: {columns}x{rows} exceeds the {MAX_SAMPLES_PER_AXIS}-sample per-axis \
                 ceiling — split it into tiles"
            )));
        }
        if samples.len() as u64 != u64::from(columns) * u64::from(rows) {
            return Err(PipelineError::Unsupported(format!(
                "heightfield: {} samples for a {columns}x{rows} grid",
                samples.len()
            )));
        }
        Ok(Heightfield {
            columns,
            rows,
            samples,
            sidecar,
            splat: None,
        })
    }

    /// Attach an RGBA8 splat map (`columns * rows * 4` bytes, R=layer0 .. A=layer3), normalizing
    /// every texel and refusing what cannot be cooked honestly: weight on a palette slot the
    /// sidecar leaves unused (the painter referenced a material the tile does not carry), and a
    /// texel with no weight on any used slot (nothing to normalize — guessing a layer would paint
    /// the ground with a material nobody chose).
    pub fn with_splat(
        mut self,
        columns: u32,
        rows: u32,
        rgba: &[u8],
    ) -> Result<Self, PipelineError> {
        let bad = |msg: String| PipelineError::Unsupported(format!("heightfield splat map: {msg}"));
        if self.sidecar.layers[0].is_none() {
            return Err(bad("the sidecar names no `layer0`".to_string()));
        }
        if columns < 1 || rows < 1 || columns > MAX_SAMPLES_PER_AXIS || rows > MAX_SAMPLES_PER_AXIS
        {
            return Err(bad(format!(
                "{columns}x{rows} texels is outside 1..={MAX_SAMPLES_PER_AXIS} per axis"
            )));
        }
        if rgba.len() as u64 != u64::from(columns) * u64::from(rows) * 4 {
            return Err(bad(format!(
                "{} bytes for a {columns}x{rows} RGBA grid",
                rgba.len()
            )));
        }
        let mut weights = Vec::with_capacity(rgba.len());
        for (t, texel) in rgba.as_chunks::<4>().0.iter().enumerate() {
            let (i, j) = (t as u32 % columns, t as u32 / columns);
            let mut raw = [0u32; SPLAT_LAYER_COUNT];
            for k in 0..SPLAT_LAYER_COUNT {
                if self.sidecar.layers[k].is_none() {
                    if texel[k] != 0 {
                        return Err(bad(format!(
                            "texel ({i}, {j}) has weight {} on unused layer {k}",
                            texel[k]
                        )));
                    }
                } else {
                    raw[k] = u32::from(texel[k]);
                }
            }
            if raw.iter().sum::<u32>() == 0 {
                return Err(bad(format!(
                    "texel ({i}, {j}) has no weight on any used layer"
                )));
            }
            weights.extend_from_slice(&normalize_weights(raw));
        }
        self.splat = Some(SplatWeights {
            columns,
            rows,
            weights,
        });
        Ok(self)
    }

    /// Load a source height map and its sidecar (`<source>.toml`). A `.png` must be 16-bit
    /// grayscale; a `.r16` is headerless little-endian u16, sized by the sidecar's
    /// `columns`/`rows`.
    ///
    /// 8-bit PNGs are REFUSED rather than widened: 256 levels over a 200 m range is a 78 cm
    /// staircase, and silently cooking that as terrain is a bug report waiting to be misfiled
    /// against the physics.
    pub fn from_file(source: &Path) -> Result<Self, PipelineError> {
        let hf = Self::height_from_file(source)?;
        let splat_path = source.with_extension("splat.png");
        let has_palette = hf.sidecar.layers.iter().any(Option::is_some);
        match (has_palette, splat_path.exists()) {
            (false, false) => Ok(hf),
            (false, true) => Err(PipelineError::Unsupported(format!(
                "heightfield {}: {} exists but the sidecar names no layers — add `layer0 = 0x...` \
                 or remove the splat map",
                source.display(),
                splat_path.display()
            ))),
            (true, false) => Err(PipelineError::Unsupported(format!(
                "heightfield {}: the sidecar names layers but {} does not exist",
                source.display(),
                splat_path.display()
            ))),
            (true, true) => {
                let img = image::open(&splat_path).map_err(PipelineError::Image)?;
                let image::DynamicImage::ImageRgba8(buf) = img else {
                    return Err(PipelineError::Unsupported(format!(
                        "heightfield splat map {}: expected an 8-bit RGBA PNG, found {:?}",
                        splat_path.display(),
                        img.color()
                    )));
                };
                let (w, h) = buf.dimensions();
                hf.with_splat(w, h, buf.as_raw())
            }
        }
    }

    fn height_from_file(source: &Path) -> Result<Self, PipelineError> {
        let sidecar_path = source.with_extension("toml");
        let text = std::fs::read_to_string(&sidecar_path).map_err(|e| {
            PipelineError::Unsupported(format!(
                "heightfield {}: cannot read sidecar {}: {e}",
                source.display(),
                sidecar_path.display()
            ))
        })?;
        let sidecar = HeightfieldSidecar::parse(&text)?;
        let ext = source
            .extension()
            .and_then(|e| e.to_str())
            .map(str::to_ascii_lowercase);
        match ext.as_deref() {
            Some("png") => {
                let img = image::open(source).map_err(PipelineError::Image)?;
                let image::DynamicImage::ImageLuma16(buf) = img else {
                    return Err(PipelineError::Unsupported(format!(
                        "heightfield {}: expected a 16-bit grayscale PNG, found {:?}",
                        source.display(),
                        img.color()
                    )));
                };
                let (w, h) = buf.dimensions();
                Heightfield::new(w, h, buf.into_raw(), sidecar)
            }
            Some("r16") | Some("raw") => {
                let (Some(columns), Some(rows)) = (sidecar.columns, sidecar.rows) else {
                    return Err(PipelineError::Unsupported(format!(
                        "heightfield {}: a raw source needs `columns` and `rows` in its sidecar",
                        source.display()
                    )));
                };
                let bytes = std::fs::read(source)?;
                if bytes.len() as u64 != u64::from(columns) * u64::from(rows) * 2 {
                    return Err(PipelineError::Unsupported(format!(
                        "heightfield {}: {} bytes is not {columns}x{rows} u16 samples",
                        source.display(),
                        bytes.len()
                    )));
                }
                let samples = bytes
                    .as_chunks::<2>()
                    .0
                    .iter()
                    .map(|c| u16::from_le_bytes(*c))
                    .collect();
                Heightfield::new(columns, rows, samples, sidecar)
            }
            _ => Err(PipelineError::Unsupported(format!(
                "heightfield {}: expected a 16-bit .png or a raw .r16 source",
                source.display()
            ))),
        }
    }

    /// Metres per quantisation step: the sidecar's vertical range spread over the full u16 span.
    pub fn height_scale(&self) -> f32 {
        (self.sidecar.height_max - self.sidecar.height_min) / 65535.0
    }

    /// Encode the payload (the bytes after the RMA1 header). Field by field, little-endian, in
    /// exactly the order of the C++ `HeightfieldHeaderV1` record, then the samples verbatim, then (v2)
    /// the splat block.
    pub fn encode_payload(&self) -> Vec<u8> {
        let min = self.samples.iter().copied().min().unwrap_or(0);
        let max = self.samples.iter().copied().max().unwrap_or(0);
        let mut w = ByteWriter::new();
        // v2 only when there is a splat block to write; otherwise the exact v1 bytes as before.
        w.u32(if self.splat.is_some() {
            HEIGHTFIELD_PAYLOAD_VERSION
        } else {
            HEIGHTFIELD_PAYLOAD_VERSION_V1
        });
        w.u32(self.columns);
        w.u32(self.rows);
        // Spacing = extent / cell count: size_x covers first sample to last, i.e. columns-1 cells.
        w.f32(self.sidecar.size_x / (self.columns - 1) as f32);
        w.f32(self.sidecar.size_z / (self.rows - 1) as f32);
        w.f32(self.sidecar.origin[0]);
        w.f32(self.sidecar.origin[1]);
        w.f32(self.sidecar.origin[2]);
        w.f32(self.height_scale());
        w.f32(self.sidecar.height_min);
        w.u32(TRIANGULATION_DIAGONAL_MIN_TO_MAX);
        w.u32(u32::from(min));
        w.u32(u32::from(max));
        for &q in &self.samples {
            w.u16(q);
        }
        if let Some(splat) = &self.splat {
            w.u32(splat.columns);
            w.u32(splat.rows);
            w.u32(SPLAT_LAYER_COUNT as u32);
            for layer in self.sidecar.layers {
                w.u64(layer.unwrap_or(0));
            }
            w.bytes(&splat.weights);
        }
        w.into_vec()
    }

    /// Cook to a complete RMA1 file; returns `(file_bytes, asset_id)`.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        wrap_container(
            ASSET_KIND_HEIGHTFIELD,
            HEIGHTFIELD_SCHEMA_HASH,
            &self.encode_payload(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sidecar() -> HeightfieldSidecar {
        HeightfieldSidecar::parse("size_x = 8\nsize_z = 4\nheight_min = -10\nheight_max = 55.535")
            .unwrap()
    }

    fn ramp(columns: u32, rows: u32) -> Heightfield {
        let samples = (0..columns * rows)
            .map(|k| (k * 2749 % 65536) as u16)
            .collect();
        Heightfield::new(columns, rows, samples, sidecar()).unwrap()
    }

    #[test]
    fn cook_is_byte_identical_run_to_run() {
        // The determinism proof: two independent cooks of the same source produce the same bytes
        // and hence the same content id — which is also what the cook cache and asset
        // de-duplication rely on (an id that drifted would re-mint on every cook).
        let (a, id_a) = ramp(33, 17).cook();
        let (b, id_b) = ramp(33, 17).cook();
        assert_eq!(a, b);
        assert_eq!(id_a, id_b);
        // And it is sensitive: one sample changed is a different asset.
        let mut changed = ramp(33, 17);
        changed.samples[100] ^= 1;
        assert_ne!(changed.cook().0, a);
    }

    #[test]
    fn payload_layout_matches_the_reader() {
        let hf = ramp(5, 3);
        let p = hf.encode_payload();
        assert_eq!(p.len(), 52 + 5 * 3 * 2); // 13 four-byte header fields, then the samples
        let u32_at = |o: usize| u32::from_le_bytes(p[o..o + 4].try_into().unwrap());
        let f32_at = |o: usize| f32::from_le_bytes(p[o..o + 4].try_into().unwrap());
        assert_eq!(u32_at(0), HEIGHTFIELD_PAYLOAD_VERSION_V1);
        assert_eq!((u32_at(4), u32_at(8)), (5, 3));
        assert_eq!((f32_at(12), f32_at(16)), (2.0, 2.0)); // 8 m / 4 cells, 4 m / 2 cells
        assert_eq!(f32_at(36), -10.0); // offset = height_min
        assert!((f32_at(32) - 0.001).abs() < 1e-9); // 65.535 m / 65535 steps
        assert_eq!(u32_at(40), TRIANGULATION_DIAGONAL_MIN_TO_MAX);
        let min = *hf.samples.iter().min().unwrap();
        let max = *hf.samples.iter().max().unwrap();
        assert_eq!((u32_at(44), u32_at(48)), (u32::from(min), u32::from(max)));
        // Samples verbatim — the cook never re-quantises.
        assert_eq!(u16::from_le_bytes([p[52], p[53]]), hf.samples[0]);
    }

    #[test]
    fn sidecar_refuses_typos_duplicates_and_nonsense() {
        let ok = "size_x=1\nsize_z=1\nheight_min=0\nheight_max=1\n";
        assert!(HeightfieldSidecar::parse(ok).is_ok());
        assert!(HeightfieldSidecar::parse(&format!("{ok}hieght_max = 2\n")).is_err());
        assert!(HeightfieldSidecar::parse(&format!("{ok}size_x = 2\n")).is_err());
        assert!(HeightfieldSidecar::parse("size_x=1\nsize_z=1\nheight_min=0\n").is_err());
        assert!(
            HeightfieldSidecar::parse("size_x=1\nsize_z=1\nheight_min=1\nheight_max=1").is_err()
        );
        assert!(
            HeightfieldSidecar::parse("size_x=nan\nsize_z=1\nheight_min=0\nheight_max=1").is_err()
        );
        let c =
            HeightfieldSidecar::parse(&format!("# comment\n{ok}origin_y = -3 # inline\n")).unwrap();
        assert_eq!(c.origin, [0.0, -3.0, 0.0]);
    }

    #[test]
    fn grids_that_are_not_surfaces_are_refused() {
        assert!(Heightfield::new(1, 5, vec![0; 5], sidecar()).is_err());
        assert!(Heightfield::new(2, 2, vec![0; 3], sidecar()).is_err());
        assert!(Heightfield::new(MAX_SAMPLES_PER_AXIS + 1, 2, vec![0; 32772], sidecar()).is_err());
    }

    fn palette_sidecar(layers: &str) -> HeightfieldSidecar {
        HeightfieldSidecar::parse(&format!(
            "size_x = 8\nsize_z = 4\nheight_min = -10\nheight_max = 55.535\n{layers}"
        ))
        .unwrap()
    }

    fn splat_hf(layers: &str, w: u32, h: u32, rgba: &[u8]) -> Result<Heightfield, PipelineError> {
        let samples = vec![0u16; 4 * 3];
        Heightfield::new(4, 3, samples, palette_sidecar(layers))
            .unwrap()
            .with_splat(w, h, rgba)
    }

    const THREE_LAYERS: &str = "layer0 = 0x11\nlayer1 = 0x2222\nlayer2 = 0xfedcba9876543210\n";

    #[test]
    fn apportionment_always_sums_to_255_and_keeps_normalized_texels() {
        assert_eq!(normalize_weights([1, 1, 1, 0]), [85, 85, 85, 0]);
        assert_eq!(normalize_weights([255, 255, 0, 0]), [128, 127, 0, 0]); // tie -> lowest index
        assert_eq!(normalize_weights([3, 0, 0, 0]), [255, 0, 0, 0]);
        assert_eq!(normalize_weights([100, 100, 55, 0]), [100, 100, 55, 0]); // identity
        assert_eq!(normalize_weights([0, 0, 0, 7]), [0, 0, 0, 255]);
        // Exhaustive-ish sweep: every result sums to 255, zero stays zero, and an input that
        // already sums to 255 is returned unchanged.
        for a in (0..=255u32).step_by(5) {
            for b in (0..=255u32).step_by(7) {
                for c in (0..=255u32).step_by(11) {
                    for d in [0u32, 1, 9, 100, 255] {
                        let raw = [a, b, c, d];
                        let total: u32 = raw.iter().sum();
                        if total == 0 {
                            continue;
                        }
                        let out = normalize_weights(raw);
                        assert_eq!(
                            out.iter().map(|&v| u32::from(v)).sum::<u32>(),
                            255,
                            "{raw:?}"
                        );
                        for k in 0..4 {
                            if raw[k] == 0 {
                                assert_eq!(out[k], 0, "{raw:?}");
                            }
                        }
                        if total == 255 {
                            assert_eq!(out.map(u32::from), raw, "identity {raw:?}");
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn v2_payload_layout_matches_the_reader() {
        // 3x2 splat, deliberately not the 4x3 height grid; texels differ so a transposed or
        // de-interleaved walk would fail. Texel (i, j) = [10*i + 1, 20*j + 1, 0, 0] scaled.
        let mut rgba = Vec::new();
        for j in 0..2u8 {
            for i in 0..3u8 {
                rgba.extend_from_slice(&[10 * i + 1, 20 * j + 1, 0, 0]);
            }
        }
        let hf = splat_hf("layer0 = 0x11\nlayer1 = 0x2222\n", 3, 2, &rgba).unwrap();
        let p = hf.encode_payload();
        let u32_at = |o: usize| u32::from_le_bytes(p[o..o + 4].try_into().unwrap());
        let u64_at = |o: usize| u64::from_le_bytes(p[o..o + 8].try_into().unwrap());
        assert_eq!(u32_at(0), 2);
        let b = 52 + 4 * 3 * 2; // end of the u16 sample blob
        assert_eq!(p.len(), b + 12 + 32 + 3 * 2 * 4);
        assert_eq!((u32_at(b), u32_at(b + 4), u32_at(b + 8)), (3, 2, 4));
        assert_eq!(u64_at(b + 12), 0x11);
        assert_eq!(u64_at(b + 20), 0x2222);
        assert_eq!((u64_at(b + 28), u64_at(b + 36)), (0, 0));
        let w = &p[b + 44..];
        for j in 0..2usize {
            for i in 0..3usize {
                let raw = [10 * i as u32 + 1, 20 * j as u32 + 1, 0, 0];
                let want = normalize_weights(raw);
                let at = (i + 3 * j) * 4;
                assert_eq!(&w[at..at + 4], &want, "texel ({i}, {j})");
            }
        }
        // Texel (2, 0) vs (0, 1) differ, so row-major x-fastest is what was written.
        assert_ne!(&w[8..12], &w[12..16]);
    }

    #[test]
    fn no_layers_still_emits_v1_without_a_splat_block() {
        let p = ramp(5, 3).encode_payload();
        assert_eq!(u32::from_le_bytes(p[0..4].try_into().unwrap()), 1);
        assert_eq!(p.len(), 52 + 5 * 3 * 2);
    }

    #[test]
    fn layer_keys_are_hex_only_unique_nonzero_and_need_layer0() {
        let base = "size_x=1\nsize_z=1\nheight_min=0\nheight_max=1\n";
        let parse = |extra: &str| HeightfieldSidecar::parse(&format!("{base}{extra}"));
        let ok = parse("layer0 = 0xAbC\nlayer3 = 0x1 # unused 1 and 2\n").unwrap();
        assert_eq!(ok.layers, [Some(0xabc), None, None, Some(1)]);
        assert!(parse("layer0 = 12\n").is_err()); // decimal
        assert!(parse("layer0 = 0x0\n").is_err()); // zero is the unused sentinel
        assert!(parse("layer0 = 0x\n").is_err());
        assert!(parse("layer0 = 0x1\nlayer0 = 0x2\n").is_err()); // duplicate
        assert!(parse("layer1 = 0x1\n").is_err()); // layer0 missing
        assert!(parse("layer4 = 0x1\n").is_err()); // unknown key
        assert!(parse("layer00 = 0x1\n").is_err());
        assert!(parse("layer0 = 0x1ffffffffffffffff\n").is_err()); // overflows u64
    }

    #[test]
    fn splat_refusals_name_the_texel() {
        // Weight on unused layer 3 at texel (1, 0).
        let e = splat_hf(THREE_LAYERS, 2, 1, &[255, 0, 0, 0, 0, 0, 0, 9])
            .unwrap_err()
            .to_string();
        assert!(e.contains("(1, 0)") && e.contains("unused layer 3"), "{e}");
        // All-zero texel at (0, 1).
        let e = splat_hf(THREE_LAYERS, 1, 2, &[255, 0, 0, 0, 0, 0, 0, 0])
            .unwrap_err()
            .to_string();
        assert!(e.contains("(0, 1)") && e.contains("no weight"), "{e}");
        // Wrong buffer length, zero and oversize dimensions.
        assert!(splat_hf(THREE_LAYERS, 2, 2, &[255, 0, 0, 0]).is_err());
        assert!(splat_hf(THREE_LAYERS, 0, 1, &[]).is_err());
        assert!(splat_hf(THREE_LAYERS, MAX_SAMPLES_PER_AXIS + 1, 1, &[]).is_err());
        // No palette at all.
        assert!(splat_hf("", 1, 1, &[255, 0, 0, 0]).is_err());
    }
}
