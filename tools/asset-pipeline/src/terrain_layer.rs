// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The terrain **layer** cook (M19.7a, ADR-0066): one surface a terrain splat palette can name — a
//! material, an albedo image and a height image, tiled in world space — cooked to TWO RMA1 files:
//!
//! * `<name>_albedo_height.rtex` — an ordinary RGBA8 sRGB texture with RGB = albedo and
//!   A = height (linear; see [`Texture::terrain_albedo_height`] for why sRGB is right for both);
//! * `<name>.rtl` — the `TerrainLayer` record: the material's AssetId, that texture's AssetId, the
//!   world UV scale and the height contrast. The C++ reader is `read_terrain_layer`; the byte layout
//!   is in `FORMAT.md`.
//!
//! The texture is a separate asset rather than inlined because that is how every texture reference
//! in the engine works (a content id resolved to one shared GPU upload), and because a heightfield's
//! palette may name the same layer from many tiles.
//!
//! ## The sidecar
//!
//! `<name>.terrainlayer.toml`, in the heightfield sidecar's style — a hand-parsed TOML subset, one
//! `key = value` per line, `#` comments, strict:
//!
//! ```text
//! material = 0x1f2e3d4c5b6a7988    # the cooked material's AssetId, 0x hex, nonzero
//! albedo = "gravel_albedo.png"     # 8-bit RGB/RGBA (opaque), relative to this file
//! height = "gravel_height.png"     # 8- or 16-bit grey, same size as the albedo
//! uv_scale = [2.0, 2.0]            # metres per texture repeat along world X, Z; finite, > 0
//! height_contrast = 8.0            # finite, >= 0; 0 = today's plain weighted blend
//! ```
//!
//! Every key is required (a forgotten `height_contrast` defaulting to something would be a terrain
//! that blends differently from what its author tuned, with nothing reporting it), and unknown or
//! duplicate keys are errors, for the reason `HeightfieldSidecar::parse` gives.

use std::path::{Path, PathBuf};

use crate::cooked::{
    wrap_container, ByteWriter, ASSET_KIND_TERRAIN_LAYER, TERRAIN_LAYER_SCHEMA_HASH,
};
use crate::texture::Texture;
use crate::PipelineError;

/// The sidecar's file-name suffix: `gravel.terrainlayer.toml` cooks the layer named `gravel`.
pub const SIDECAR_SUFFIX: &str = ".terrainlayer.toml";

/// A parsed `<name>.terrainlayer.toml`. Paths are exactly as written (relative to the sidecar).
#[derive(Debug, Clone, PartialEq)]
pub struct TerrainLayerSidecar {
    pub material: u64,
    pub albedo: PathBuf,
    pub height: PathBuf,
    pub uv_scale: [f32; 2],
    pub height_contrast: f32,
}

/// Strip a `#` comment, but not one inside a `"..."` string: a path may legally contain `#`.
pub(crate) fn strip_comment(line: &str) -> &str {
    let mut in_string = false;
    for (i, c) in line.char_indices() {
        match c {
            '"' => in_string = !in_string,
            '#' if !in_string => return &line[..i],
            _ => {}
        }
    }
    line
}

impl TerrainLayerSidecar {
    /// Parse the sidecar text, refusing a missing, unknown, duplicate or out-of-range key.
    pub fn parse(text: &str) -> Result<Self, PipelineError> {
        let bad = |msg: String| PipelineError::Unsupported(format!("terrain layer sidecar: {msg}"));
        let mut material = None;
        let mut albedo = None;
        let mut height = None;
        let mut uv_scale = None;
        let mut height_contrast = None;
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
            let float = |s: &str| {
                s.trim()
                    .parse::<f32>()
                    .ok()
                    .filter(|v| v.is_finite())
                    .ok_or_else(|| at(&format!("`{key}` needs finite numbers")))
            };
            let string = || {
                value
                    .strip_prefix('"')
                    .and_then(|v| v.strip_suffix('"'))
                    .filter(|v| !v.is_empty() && !v.contains('"'))
                    .map(PathBuf::from)
                    .ok_or_else(|| at(&format!("`{key}` needs a non-empty \"quoted\" path")))
            };
            let duplicate = || at(&format!("`{key}` given twice"));
            match key {
                "material" => {
                    // Hex only, nonzero — the heightfield sidecar's `layerN` rule: an AssetId is
                    // printed as hex everywhere, and 0 is the reserved "no asset" id.
                    let id = value
                        .strip_prefix("0x")
                        .filter(|h| !h.is_empty() && h.bytes().all(|c| c.is_ascii_hexdigit()))
                        .and_then(|h| u64::from_str_radix(h, 16).ok())
                        .filter(|&id| id != 0)
                        .ok_or_else(|| {
                            at("`material` needs a nonzero material AssetId in 0x hex")
                        })?;
                    if material.replace(id).is_some() {
                        return Err(duplicate());
                    }
                }
                "albedo" => {
                    if albedo.replace(string()?).is_some() {
                        return Err(duplicate());
                    }
                }
                "height" => {
                    if height.replace(string()?).is_some() {
                        return Err(duplicate());
                    }
                }
                "uv_scale" => {
                    let inner = value
                        .strip_prefix('[')
                        .and_then(|v| v.strip_suffix(']'))
                        .ok_or_else(|| at("`uv_scale` needs `[x, z]`"))?;
                    let parts: Vec<&str> = inner.split(',').collect();
                    let [x, z] = parts.as_slice() else {
                        return Err(at("`uv_scale` needs exactly two numbers, `[x, z]`"));
                    };
                    let xz = [float(x)?, float(z)?];
                    if xz.iter().any(|&v| v <= 0.0) {
                        return Err(at("`uv_scale` must be positive on both axes"));
                    }
                    if uv_scale.replace(xz).is_some() {
                        return Err(duplicate());
                    }
                }
                "height_contrast" => {
                    let c = float(value)?;
                    if c < 0.0 {
                        return Err(at("`height_contrast` must be >= 0 (0 = no height effect)"));
                    }
                    if height_contrast.replace(c).is_some() {
                        return Err(duplicate());
                    }
                }
                _ => return Err(at(&format!("unknown key `{key}`"))),
            }
        }
        let missing = |k: &str| bad(format!("missing `{k}`"));
        Ok(TerrainLayerSidecar {
            material: material.ok_or_else(|| missing("material"))?,
            albedo: albedo.ok_or_else(|| missing("albedo"))?,
            height: height.ok_or_else(|| missing("height"))?,
            uv_scale: uv_scale.ok_or_else(|| missing("uv_scale"))?,
            height_contrast: height_contrast.ok_or_else(|| missing("height_contrast"))?,
        })
    }
}

/// The `TerrainLayer` record, ready to encode. Mirrors the C++ `TerrainLayerAsset`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TerrainLayer {
    pub material: u64,
    /// The AssetId of the cooked albedo+height texture.
    pub albedo_height: u64,
    pub uv_scale: [f32; 2],
    pub height_contrast: f32,
}

impl TerrainLayer {
    /// Encode into a complete RMA1 file, returning `(bytes, asset_id)`. The field order IS the wire
    /// format: it mirrors the C++ `TerrainLayerV1` record (which fingerprints it) and
    /// `decode_terrain_layer` (which reads it). A fixed 28-byte payload.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let mut p = ByteWriter::new();
        p.u64(self.material);
        p.u64(self.albedo_height);
        p.f32(self.uv_scale[0]);
        p.f32(self.uv_scale[1]);
        p.f32(self.height_contrast);
        wrap_container(
            ASSET_KIND_TERRAIN_LAYER,
            TERRAIN_LAYER_SCHEMA_HASH,
            &p.into_vec(),
        )
    }
}

/// Both cooked files of one layer: the packed texture and the record that references it.
#[derive(Debug, Clone)]
pub struct CookedTerrainLayer {
    /// The layer's name: the sidecar's file name without [`SIDECAR_SUFFIX`].
    pub name: String,
    pub texture: (Vec<u8>, u64),
    pub layer: (Vec<u8>, u64),
}

impl CookedTerrainLayer {
    /// The texture's cooked file name, `<name>_albedo_height.rtex`.
    pub fn texture_file_name(&self) -> String {
        format!("{}_albedo_height.rtex", self.name)
    }

    /// The layer's cooked file name, `<name>.rtl`.
    pub fn layer_file_name(&self) -> String {
        format!("{}.rtl", self.name)
    }
}

/// Cook a layer from its `<name>.terrainlayer.toml`: parse it, load and pack the two images
/// (resolved relative to the sidecar's directory), cook the texture, then the record naming it.
/// Pure — nothing is written; the CLI writes both files.
pub fn cook_terrain_layer(sidecar_path: &Path) -> Result<CookedTerrainLayer, PipelineError> {
    let name = sidecar_path
        .file_name()
        .and_then(|f| f.to_str())
        .and_then(|f| f.strip_suffix(SIDECAR_SUFFIX))
        .filter(|n| !n.is_empty())
        .ok_or_else(|| {
            PipelineError::Unsupported(format!(
                "terrain layer {}: expected a `<name>{SIDECAR_SUFFIX}` file",
                sidecar_path.display()
            ))
        })?
        .to_string();
    let text = std::fs::read_to_string(sidecar_path)?;
    let sidecar = TerrainLayerSidecar::parse(&text)?;
    let dir = sidecar_path.parent().unwrap_or(Path::new(""));
    let texture = Texture::terrain_albedo_height_from_files(
        &dir.join(&sidecar.albedo),
        &dir.join(&sidecar.height),
    )?
    .cook();
    let layer = TerrainLayer {
        material: sidecar.material,
        albedo_height: texture.1,
        uv_scale: sidecar.uv_scale,
        height_contrast: sidecar.height_contrast,
    }
    .cook();
    Ok(CookedTerrainLayer {
        name,
        texture,
        layer,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cooked::read_header;

    const GOOD: &str = r#"
# a comment line
material = 0x1f2e3d4c5b6a7988   # trailing comment
albedo = "a#b/albedo.png"       # a `#` inside a string is part of the path
height = "height.png"
uv_scale = [2.5, 4.0]
height_contrast = 8.0
"#;

    fn err_of(text: &str) -> String {
        TerrainLayerSidecar::parse(text).unwrap_err().to_string()
    }

    /// GOOD with the line starting `key` replaced by `line` (or removed if `line` is empty).
    fn with(key: &str, line: &str) -> String {
        GOOD.lines()
            .map(|l| if l.starts_with(key) { line } else { l })
            .collect::<Vec<_>>()
            .join("\n")
    }

    #[test]
    fn a_good_sidecar_parses_every_field() {
        let sc = TerrainLayerSidecar::parse(GOOD).unwrap();
        assert_eq!(sc.material, 0x1f2e_3d4c_5b6a_7988);
        assert_eq!(sc.albedo, PathBuf::from("a#b/albedo.png"));
        assert_eq!(sc.height, PathBuf::from("height.png"));
        assert_eq!(sc.uv_scale, [2.5, 4.0]);
        assert_eq!(sc.height_contrast, 8.0);
        // Zero contrast is the valid "no height effect" setting, not an error.
        assert_eq!(
            TerrainLayerSidecar::parse(&with("height_contrast", "height_contrast = 0"))
                .unwrap()
                .height_contrast,
            0.0
        );
    }

    #[test]
    fn every_missing_key_is_refused_by_name() {
        for key in [
            "material",
            "albedo",
            "height",
            "uv_scale",
            "height_contrast",
        ] {
            let e = err_of(&with(key, ""));
            assert!(e.contains(&format!("missing `{key}`")), "{key}: {e}");
        }
    }

    #[test]
    fn a_zero_or_malformed_material_id_is_refused() {
        for v in [
            "0x0",
            "0x",
            "0",
            "1234",
            "0xZZ",
            "0x1_0",
            "0x10000000000000000",
        ] {
            let e = err_of(&with("material", &format!("material = {v}")));
            assert!(e.contains("nonzero material AssetId"), "{v}: {e}");
        }
    }

    #[test]
    fn a_non_finite_or_non_positive_uv_scale_is_refused() {
        for v in [
            "[0, 1]",
            "[1, 0]",
            "[-1, 1]",
            "[1, -2]",
            "[-0.0, 1]",
            "[nan, 1]",
            "[1, inf]",
            "[inf, 1]",
        ] {
            let e = err_of(&with("uv_scale", &format!("uv_scale = {v}")));
            assert!(
                e.contains("positive") || e.contains("finite"),
                "{v} was accepted or refused for the wrong reason: {e}"
            );
        }
        for v in ["[1]", "[1, 2, 3]", "1, 2", "[1, 2"] {
            let e = err_of(&with("uv_scale", &format!("uv_scale = {v}")));
            assert!(e.contains("uv_scale"), "{v}: {e}");
        }
    }

    #[test]
    fn a_negative_or_non_finite_contrast_is_refused() {
        for v in ["-1", "-0.001", "nan", "inf", "-inf"] {
            let e = err_of(&with("height_contrast", &format!("height_contrast = {v}")));
            assert!(
                e.contains(">= 0") || e.contains("finite"),
                "{v} was accepted or refused for the wrong reason: {e}"
            );
        }
    }

    #[test]
    fn unknown_duplicate_and_unquoted_keys_are_refused() {
        let e = err_of(&format!("{GOOD}\nheight_contrst = 1.0"));
        assert!(e.contains("unknown key `height_contrst`"), "{e}");
        let e = err_of(&format!("{GOOD}\nuv_scale = [1, 1]"));
        assert!(e.contains("given twice"), "{e}");
        let e = err_of(&with("albedo", "albedo = albedo.png"));
        assert!(e.contains("quoted"), "{e}");
        let e = err_of(&with("albedo", "albedo = \"\""));
        assert!(e.contains("quoted"), "{e}");
        let e = err_of(&format!("{GOOD}\njust words"));
        assert!(e.contains("key = value"), "{e}");
    }

    #[test]
    fn the_record_is_28_bytes_in_wire_order() {
        let layer = TerrainLayer {
            material: 0x1122_3344_5566_7788,
            albedo_height: 0x99aa_bbcc_ddee_ff00,
            uv_scale: [2.5, 4.0],
            height_contrast: 8.0,
        };
        let (bytes, id) = layer.cook();
        assert_eq!(
            layer.cook(),
            (bytes.clone(), id),
            "cook must be deterministic"
        );
        let (header, payload) = read_header(&bytes).unwrap();
        assert_eq!(header.asset_kind, ASSET_KIND_TERRAIN_LAYER);
        assert_eq!(header.type_schema_hash, TERRAIN_LAYER_SCHEMA_HASH);
        let mut expected = Vec::new();
        expected.extend_from_slice(&0x1122_3344_5566_7788u64.to_le_bytes());
        expected.extend_from_slice(&0x99aa_bbcc_ddee_ff00u64.to_le_bytes());
        expected.extend_from_slice(&2.5f32.to_le_bytes());
        expected.extend_from_slice(&4.0f32.to_le_bytes());
        expected.extend_from_slice(&8.0f32.to_le_bytes());
        assert_eq!(payload, expected.as_slice());
    }

    #[test]
    fn schema_hash_is_the_constant_the_engine_pins() {
        // cooked_terrain_layer_test.cpp pins terrain_layer_schema_hash() to this same value.
        assert_eq!(TERRAIN_LAYER_SCHEMA_HASH, 0xFE78_32A2_7003_413C);
        assert_eq!(ASSET_KIND_TERRAIN_LAYER, 10);
    }

    #[test]
    fn the_sidecar_name_must_carry_the_suffix() {
        let e = cook_terrain_layer(Path::new("gravel.toml"))
            .unwrap_err()
            .to_string();
        assert!(e.contains(SIDECAR_SUFFIX), "{e}");
        let e = cook_terrain_layer(Path::new(SIDECAR_SUFFIX))
            .unwrap_err()
            .to_string();
        assert!(e.contains(SIDECAR_SUFFIX), "{e}");
    }
}
