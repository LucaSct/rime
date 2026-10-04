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
//! ```
//!
//! A hand-rolled parser rather than a TOML dependency: the format is nine numbers, and a new crate
//! for nine numbers is a supply-chain cost with no reader to show for it.

use std::path::Path;

use crate::cooked::{wrap_container, ByteWriter, ASSET_KIND_HEIGHTFIELD, HEIGHTFIELD_SCHEMA_HASH};
use crate::PipelineError;

/// The payload's own version (the first field) — the C++ `kHeightfieldPayloadVersion`.
pub const HEIGHTFIELD_PAYLOAD_VERSION: u32 = 1;

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
        };
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
        })
    }

    /// Load a source height map and its sidecar (`<source>.toml`). A `.png` must be 16-bit
    /// grayscale; a `.r16` is headerless little-endian u16, sized by the sidecar's
    /// `columns`/`rows`.
    ///
    /// 8-bit PNGs are REFUSED rather than widened: 256 levels over a 200 m range is a 78 cm
    /// staircase, and silently cooking that as terrain is a bug report waiting to be misfiled
    /// against the physics.
    pub fn from_file(source: &Path) -> Result<Self, PipelineError> {
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
    /// exactly the order of the C++ `HeightfieldHeaderV1` record, then the samples verbatim.
    pub fn encode_payload(&self) -> Vec<u8> {
        let min = self.samples.iter().copied().min().unwrap_or(0);
        let max = self.samples.iter().copied().max().unwrap_or(0);
        let mut w = ByteWriter::new();
        w.u32(HEIGHTFIELD_PAYLOAD_VERSION);
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
        assert_eq!(u32_at(0), HEIGHTFIELD_PAYLOAD_VERSION);
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
}
